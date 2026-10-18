/* js - JavaScript on ThreadOS
 *
 *   js                 a console: a line in, its value out
 *   js file.js [args]  runs a script (its arguments in scriptArgs)
 *   js -e code...      runs the words after -e as one line of code
 *
 * The engine is QuickJS (third_party/quickjs), the C library musl
 * (libc/) - this file is only the host around them: it gives the
 * engine a console, timers, files and a clock, and runs the event loop.
 * Most of that is written in JavaScript (prelude.js, built into this
 * program); what is here is what only C can do.
 *
 * The exit code: 0, or 1 after an exception nobody caught - or what
 * the script passed to exit().
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "quickjs.h"
#include "prelude.h"                 /* prelude_js[], made from prelude.js */

/* QuickJS checks its own recursion against this - and ThreadOS gives a
   program a stack of 4 MiB (USER64_STACK_MAX), part of which the C
   side uses itself. Too deep a recursion is then a RangeError the
   script can catch, not a page fault that kills the program. */
#define JS_STACK (3584 * 1024)

static JSValue host_obj;             /* what prelude.js added hooks to */
static int exit_code;

/* --- What the prelude gets ----------------------------------------- */

static JSValue h_write(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv)
{
	int fd = 1;
	size_t len;
	JS_ToInt32(ctx, &fd, argv[0]);
	const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
	if (!s)
		return JS_EXCEPTION;
	FILE *f = fd == 2 ? stderr : stdout;
	if (fd == 2)
		fflush(stdout);              /* the order they were said in */
	fwrite(s, 1, len, f);
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static JSValue h_now(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv)
{
	return JS_NewFloat64(ctx, now_ms());
}

static char *slurp(const char *name, size_t *len)
{
	FILE *f = fopen(name, "rb");
	if (!f)
		return NULL;
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap + 1);
	size_t got;
	while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
		n += got;
		if (n == cap) {
			char *more = realloc(buf, cap * 2 + 1);
			if (!more) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = more;
			cap *= 2;
		}
	}
	fclose(f);
	if (buf) {
		buf[n] = 0;
		*len = n;
	}
	return buf;
}

static JSValue h_read_file(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
	const char *name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;
	size_t len;
	char *buf = slurp(name, &len);
	if (!buf) {
		int e = errno;
		JSValue err = JS_ThrowTypeError(ctx, "readFile '%s': %s", name,
		                                strerror(e));
		JS_FreeCString(ctx, name);
		return err;
	}
	JS_FreeCString(ctx, name);
	JSValue s = JS_NewStringLen(ctx, buf, len);
	free(buf);
	return s;
}

static JSValue h_write_file(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
	const char *name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
	if (!s) {
		JS_FreeCString(ctx, name);
		return JS_EXCEPTION;
	}
	FILE *f = fopen(name, "wb");
	size_t put = f ? fwrite(s, 1, len, f) : 0;
	int bad = !f || fclose(f) != 0 || put != len;
	int e = errno;
	JS_FreeCString(ctx, s);
	JSValue r = bad ? JS_ThrowTypeError(ctx, "writeFile '%s': %s", name,
	                                    strerror(e))
	                : JS_NewInt64(ctx, (int64_t)len);
	JS_FreeCString(ctx, name);
	return r;
}

static JSValue h_exit(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv)
{
	int code = 0;
	if (argc > 0)
		JS_ToInt32(ctx, &code, argv[0]);
	fflush(stdout);
	exit(code);
}

static JSValue h_promise_state(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
	JSValue a = JS_NewArray(ctx);
	int st = JS_PromiseState(ctx, argv[0]);
	JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, st));
	JS_SetPropertyUint32(ctx, a, 1, st == JS_PROMISE_PENDING ? JS_UNDEFINED
	                     : JS_PromiseResult(ctx, argv[0]));
	return a;
}

/* --- Running code -------------------------------------------------- */

static JSValue call_hook(JSContext *ctx, const char *name, int argc,
                         JSValueConst *argv)
{
	JSValue fn = JS_GetPropertyStr(ctx, host_obj, name);
	JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
	JS_FreeValue(ctx, fn);
	return r;
}

/* As the console shows a value: through the same inspect as
   console.log, with strings in quotes */
static void show(JSContext *ctx, JSValueConst v, FILE *out)
{
	JSValueConst args[2] = { v, JS_TRUE };
	JSValue s = call_hook(ctx, "inspect", 2, args);
	const char *str = JS_IsException(s) ? NULL : JS_ToCString(ctx, s);
	if (str) {
		fprintf(out, "%s\n", str);
		JS_FreeCString(ctx, str);
	} else {
		JS_FreeValue(ctx, JS_GetException(ctx));
		fprintf(out, "?\n");
	}
	JS_FreeValue(ctx, s);
}

static void report(JSContext *ctx)
{
	JSValue e = JS_GetException(ctx);
	fflush(stdout);
	fprintf(stderr, "Uncaught ");
	if (JS_IsError(ctx, e)) {
		const char *msg = JS_ToCString(ctx, e);
		JSValue st = JS_GetPropertyStr(ctx, e, "stack");
		const char *stack = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
		fprintf(stderr, "%s\n%s", msg ? msg : "?", stack ? stack : "");
		if (stack)
			JS_FreeCString(ctx, stack);
		if (msg)
			JS_FreeCString(ctx, msg);
		JS_FreeValue(ctx, st);
	} else {
		show(ctx, e, stderr);
	}
	JS_FreeValue(ctx, e);
	exit_code = 1;
}

static void run_jobs(JSContext *ctx)
{
	JSContext *c;
	int r;
	while ((r = JS_ExecutePendingJob(JS_GetRuntime(ctx), &c)) != 0)
		if (r < 0)
			report(c);
}

/* The event loop: promise jobs first, then the earliest timer, until
   there is neither */
static void event_loop(JSContext *ctx)
{
	for (;;) {
		run_jobs(ctx);
		JSValue r = call_hook(ctx, "nextTimer", 0, NULL);
		int32_t wait = -1;
		JS_ToInt32(ctx, &wait, r);
		JS_FreeValue(ctx, r);
		if (wait < 0)
			break;
		if (wait > 0) {
			struct timespec ts = { wait / 1000, (wait % 1000) * 1000000L };
			nanosleep(&ts, NULL);
		}
		r = call_hook(ctx, "fireTimer", 0, NULL);
		if (JS_IsException(r))
			report(ctx);
		JS_FreeValue(ctx, r);
	}
}

static int eval(JSContext *ctx, const char *code, size_t len,
                const char *file, int show_result)
{
	JSValue r = JS_Eval(ctx, code, len, file, JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(r)) {
		report(ctx);
		return -1;
	}
	if (show_result)
		show(ctx, r, stdout);
	JS_FreeValue(ctx, r);
	return 0;
}

static void repl(JSContext *ctx)
{
	char line[1024];
	printf("Welcome to QuickJS %s on ThreadOS.\n"
	       "Type \".help\" for more information.\n", CONFIG_VERSION);
	for (;;) {
		printf("> ");
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin))
			break;
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!strcmp(line, ".exit"))
			break;
		if (!strcmp(line, ".help")) {
			printf(".exit     leave the console (Ctrl+D does the same)\n"
			       "Anything else is JavaScript; its value is shown.\n"
			       "console.log, setTimeout, readFile(name),\n"
			       "writeFile(name, text) and exit(code) are there.\n");
			continue;
		}
		if (!n)
			continue;
		eval(ctx, line, n, "<repl>", 1);
		event_loop(ctx);
	}
	printf("\n");
	exit_code = 0;                   /* a mistake typed is not a failure */
}

static JSContext *make_context(JSRuntime *rt, int argc, char **argv,
                               int first_arg)
{
	JSContext *ctx = JS_NewContext(rt);
	if (!ctx)
		return NULL;
	JSValue global = JS_GetGlobalObject(ctx);
	host_obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, host_obj, "write",
	                  JS_NewCFunction(ctx, h_write, "write", 2));
	JS_SetPropertyStr(ctx, host_obj, "now",
	                  JS_NewCFunction(ctx, h_now, "now", 0));
	JS_SetPropertyStr(ctx, host_obj, "readFile",
	                  JS_NewCFunction(ctx, h_read_file, "readFile", 1));
	JS_SetPropertyStr(ctx, host_obj, "writeFile",
	                  JS_NewCFunction(ctx, h_write_file, "writeFile", 2));
	JS_SetPropertyStr(ctx, host_obj, "exit",
	                  JS_NewCFunction(ctx, h_exit, "exit", 1));
	JS_SetPropertyStr(ctx, host_obj, "promiseState",
	                  JS_NewCFunction(ctx, h_promise_state, "promiseState", 1));

	JSValue args = JS_NewArray(ctx);
	for (int i = first_arg; i < argc; i++)
		JS_SetPropertyUint32(ctx, args, i - first_arg,
		                     JS_NewString(ctx, argv[i]));
	JS_SetPropertyStr(ctx, global, "scriptArgs", args);

	JSValue fn = JS_Eval(ctx, prelude_js, sizeof(prelude_js) - 1,
	                     "<prelude>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(fn)) {
		report(ctx);
		return NULL;
	}
	JSValueConst pa[2] = { global, host_obj };
	JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, pa);
	if (JS_IsException(r)) {
		report(ctx);
		return NULL;
	}
	JS_FreeValue(ctx, r);
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	return ctx;
}

int main(int argc, char **argv)
{
	/* A console wants to see each line as it is printed; a file or
	   a pipe is better served in blocks, but the terminal is a file
	   too (user/gterm.c) */
	setvbuf(stdout, NULL, _IOLBF, 0);

	JSRuntime *rt = JS_NewRuntime();
	if (!rt) {
		fprintf(stderr, "js: no memory for the engine\n");
		return 1;
	}
	JS_SetMaxStackSize(rt, JS_STACK);

	if (argc > 1 && !strcmp(argv[1], "-e")) {
		/* The words after -e are one line: a shell that splits on
		   spaces and knows no quotes still lets one say anything */
		size_t len = 0;
		for (int i = 2; i < argc; i++)
			len += strlen(argv[i]) + 1;
		char *code = calloc(1, len + 1);
		for (int i = 2; i < argc; i++) {
			if (i > 2)
				strcat(code, " ");
			strcat(code, argv[i]);
		}
		JSContext *ctx = make_context(rt, argc, argv, 2);
		if (!ctx)
			return 1;
		eval(ctx, code, strlen(code), "<cmdline>", 0);
		event_loop(ctx);
		free(code);
	} else if (argc > 1) {
		size_t len;
		char *code = slurp(argv[1], &len);
		if (!code) {
			fprintf(stderr, "js: cannot read '%s': %s\n", argv[1],
			        strerror(errno));
			return 2;
		}
		JSContext *ctx = make_context(rt, argc, argv, 1);
		if (!ctx)
			return 1;
		eval(ctx, code, len, argv[1], 0);
		event_loop(ctx);
		free(code);
	} else {
		JSContext *ctx = make_context(rt, argc, argv, 1);
		if (!ctx)
			return 1;
		repl(ctx);
	}
	fflush(stdout);
	return exit_code;
}
