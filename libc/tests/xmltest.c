/* xmltest - libxml2 on ThreadOS
 *
 * The XML parser a web engine reads XHTML, SVG and XML documents with.
 * Every file named on the command line is parsed, and its tree said in
 * a canonical form - an element a line, its namespace and name, its
 * attributes sorted, the text before its first child and after its end
 * - that scripts/tests/t_xml.py works out with another parser, Python's
 * ElementTree (expat), from the same files. A file that is not XML, or
 * that would expand into gigabytes, is an error said as such.
 *
 * 'queries' (the last argument, if it ends in .q) lists XPath
 * expressions, a line each: file, expression, then prefix=URI pairs,
 * separated by tabs; each is evaluated and its elements said by name
 * and text, which t_xml.py also gets from ElementTree's paths.
 *
 * And of its own: every document, written out again by libxml2 and
 * parsed once more, has the same tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

static void quiet(void *ctx, const xmlError *err)
{
    (void)ctx;
    (void)err;
}

/* Text as one line: what is not printable ASCII or is a backslash is
   escaped as Python's 'unicode_escape' would show the bytes */
static void put_text(FILE *out, const xmlChar *s)
{
    fputc('"', out);
    for (; s && *s; s++) {
        unsigned char c = *s;
        if (c == '\\')
            fputs("\\\\", out);
        else if (c == '"')
            fputs("\\\"", out);
        else if (c == '\n')
            fputs("\\n", out);
        else if (c == '\t')
            fputs("\\t", out);
        else if (c == '\r')
            fputs("\\r", out);
        else if (c < 0x20 || c >= 0x7F)
            fprintf(out, "\\x%02x", c);
        else
            fputc(c, out);
    }
    fputc('"', out);
}

/* What ElementTree calls an element's text: the text (and CDATA) among
   its children before the first element, comments and processing
   instructions skipped over */
static xmlChar *text_before(xmlNodePtr first)
{
    xmlChar *t = xmlStrdup((const xmlChar *)"");
    for (xmlNodePtr n = first; n && n->type != XML_ELEMENT_NODE; n = n->next)
        if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE)
            t = xmlStrcat(t, n->content);
    return t;
}

static const char *ns_of(xmlNsPtr ns)
{
    return ns && ns->href ? (const char *)ns->href : "";
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void dump(FILE *out, xmlNodePtr e, int depth)
{
    fprintf(out, "%*s{%s}%s", depth * 2, "", ns_of(e->ns), e->name);
    char *attrs[64];
    int n = 0;
    for (xmlAttrPtr a = e->properties; a && n < 64; a = a->next) {
        xmlChar *v = xmlNodeListGetString(e->doc, a->children, 1);
        size_t len = strlen(ns_of(a->ns)) + strlen((char *)a->name) +
                     (v ? strlen((char *)v) : 0) + 8;
        attrs[n] = malloc(len);
        snprintf(attrs[n], len, "{%s}%s=%s", ns_of(a->ns), a->name,
                 v ? (char *)v : "");
        xmlFree(v);
        n++;
    }
    qsort(attrs, n, sizeof(*attrs), by_name);
    for (int i = 0; i < n; i++) {
        fputc(' ', out);
        char *eq = strchr(attrs[i], '=');
        fwrite(attrs[i], 1, eq - attrs[i] + 1, out);
        put_text(out, (xmlChar *)eq + 1);
        free(attrs[i]);
    }
    xmlChar *t = text_before(e->children);
    fputs(" text=", out);
    put_text(out, t);
    xmlFree(t);
    t = text_before(e->next);
    fputs(" tail=", out);
    put_text(out, depth ? t : (const xmlChar *)"");
    xmlFree(t);
    fputc('\n', out);
    for (xmlNodePtr c = e->children; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE)
            dump(out, c, depth + 1);
}

static char *tree_of(xmlDocPtr doc)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    dump(out, xmlDocGetRootElement(doc), 0);
    fclose(out);
    return buf;
}

/* Default attributes from the DTD and its internal entities, as expat
   gives them; nothing from the network, nothing from other files */
#define OPTIONS (XML_PARSE_NOENT | XML_PARSE_DTDATTR | XML_PARSE_NONET | \
                 XML_PARSE_NOERROR | XML_PARSE_NOWARNING)

static xmlDocPtr parse(const char *path)
{
    return xmlReadFile(path, NULL, OPTIONS);
}

static void queries(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("%s: cannot be read\n", path);
        return;
    }
    char line[1024];
    int number = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        number++;
        char *fields[16];
        int nf = 0;
        for (char *p = strtok(line, "\t"); p && nf < 16; p = strtok(NULL, "\t"))
            fields[nf++] = p;
        if (nf < 2)
            continue;
        xmlDocPtr doc = parse(fields[0]);
        if (!doc) {
            printf("query %d: no document\n", number);
            continue;
        }
        xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
        for (int i = 2; i < nf; i++) {
            char *eq = strchr(fields[i], '=');
            if (eq) {
                *eq = 0;
                xmlXPathRegisterNs(ctx, (xmlChar *)fields[i],
                                   (xmlChar *)eq + 1);
            }
        }
        xmlXPathObjectPtr r = xmlXPathEvalExpression((xmlChar *)fields[1], ctx);
        printf("query %d:", number);
        if (!r)
            printf(" an error");
        else if (r->type == XPATH_NODESET) {
            int n = r->nodesetval ? r->nodesetval->nodeNr : 0;
            printf(" %d", n);
            for (int i = 0; i < n; i++) {
                xmlNodePtr e = r->nodesetval->nodeTab[i];
                xmlChar *t = text_before(e->children);
                printf(" %s=", e->name);
                put_text(stdout, t);
                xmlFree(t);
            }
        } else if (r->type == XPATH_NUMBER)
            printf(" number %g", r->floatval);
        printf("\n");
        xmlXPathFreeObject(r);
        xmlXPathFreeContext(ctx);
        xmlFreeDoc(doc);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    xmlSetStructuredErrorFunc(NULL, quiet);
    int again = 1, docs = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n > 2 && !strcmp(argv[i] + n - 2, ".q")) {
            queries(argv[i]);
            continue;
        }
        xmlDocPtr doc = parse(argv[i]);
        if (!doc) {
            printf("%s: an error, said as such\n", argv[i]);
            continue;
        }
        char *tree = tree_of(doc);
        printf("%s:\n%s", argv[i], tree);
        /* written out and read again */
        xmlChar *text;
        int len;
        xmlDocDumpMemoryEnc(doc, &text, &len, "UTF-8");
        xmlDocPtr back = xmlReadMemory((char *)text, len, "again.xml", NULL,
                                       OPTIONS);
        char *tree2 = back ? tree_of(back) : NULL;
        again &= tree2 && !strcmp(tree, tree2);
        docs++;
        free(tree);
        free(tree2);
        xmlFree(text);
        if (back)
            xmlFreeDoc(back);
        xmlFreeDoc(doc);
    }
    printf("%d documents written out and read again have the same tree: %s\n",
           docs, again ? "yes" : "no");
    printf("~libxml2 %s\n", xmlParserVersion);
    xmlCleanupParser();
    return 0;
}
