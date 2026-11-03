// CSS grid layout for litehtml, which has none.
//
// litehtml is third-party code kept unchanged (third_party/litehtml); the
// patches in apps/browser/patches make "display: grid" a flex container
// with css().m_grid set, so that litehtml treats a grid as it treats flex
// for all but the layout itself (its items blockified, inline text wrapped
// in anonymous blocks, a formatting context of its own), and hand the
// layout to render_item_flex::_render_grid, here.
//
// What is done is what sites use: track lists with px, %, fr, auto,
// min-content, max-content, minmax(), fit-content() and repeat() (a count,
// auto-fill, auto-fit), named lines, grid-template-areas, placement by line
// number, span, name and area, row-major auto placement, gaps, implicit
// tracks from grid-auto-rows/-columns, and justify-/align-self. Track sizes
// are worked out in one pass, not by the full algorithm of the
// specification: a track takes the largest of what the items in it alone
// need, fr shares what is left, and an item spanning auto tracks widens
// the last of them if they are too narrow for it.

#include "html.h"
#include "render_flex.h"
#include "html_tag.h"
#include "document.h"
#include "css_parser.h"

#include <algorithm>
#include <map>

namespace litehtml
{
    namespace
    {
        struct track
        {
            enum kind_t
            {
                fixed,
                fr,
                autos
            } kind      = autos;
            pixel_t min = 0;    // fixed: the size; else the least it may be
            pixel_t max = -1;   // fit-content()'s limit; -1: none
            float   flex = 0;   // fr
            pixel_t size = 0;
        };

        struct track_list
        {
            std::vector<track>                       tracks;
            std::map<std::string, std::vector<int>> lines; // name -> line indexes, 0-based
            int                                      fit_from = -1, fit_to = -1; // auto-fit's range
        };

        pixel_t length_of(const css_token& tok, pixel_t base, const document::ptr& doc, const font_metrics& fm,
                          bool* ok)
        {
            css_length l;
            *ok = l.from_token(tok, f_length_percentage | f_positive) && !l.is_predefined();
            if(!*ok)
            {
                return 0;
            }
            return doc->to_pixels(l, fm, base);
        }

        // One track size: <length-percentage>, <flex>, auto, min-content,
        // max-content, minmax(), fit-content(). False if it is none of them.
        bool parse_track(const css_token& tok, pixel_t base, const document::ptr& doc, const font_metrics& fm,
                         track& t)
        {
            bool ok = false;
            if(tok.type == DIMENSION && lowcase(tok.unit()) == "fr")
            {
                t.kind = track::fr;
                t.flex = std::max(0.0f, tok.n.number);
                return true;
            }
            std::string id = tok.ident();
            if(id == "auto" || id == "min-content" || id == "max-content")
            {
                t.kind = track::autos;
                return true;
            }
            if(tok.type == CV_FUNCTION)
            {
                std::string fn   = lowcase(tok.name());
                auto        args = parse_comma_separated_list(tok.value);
                if(fn == "minmax" && args.size() == 2 && args[0].size() == 1 && args[1].size() == 1)
                {
                    track lo, hi;
                    if(!parse_track(args[0][0], base, doc, fm, lo) || !parse_track(args[1][0], base, doc, fm, hi))
                    {
                        return false;
                    }
                    t      = hi;
                    t.min  = lo.kind == track::fixed ? lo.min : 0_px;
                    if(hi.kind == track::fixed)
                    {
                        // minmax(a, b) of two lengths: as large as allowed
                        t.min = std::max(t.min, hi.min);
                    }
                    return true;
                }
                if(fn == "fit-content" && args.size() == 1 && args[0].size() == 1)
                {
                    t.kind = track::autos;
                    t.max  = length_of(args[0][0], base, doc, fm, &ok);
                    return ok;
                }
            }
            pixel_t px = length_of(tok, base, doc, fm, &ok);
            if(ok)
            {
                t.kind = track::fixed;
                t.min  = px;
            }
            return ok;
        }

        void add_names(track_list& list, const css_token& tok)
        {
            for(const auto& name : tok.value)
            {
                if(name.type == IDENT)
                {
                    list.lines[name.name()].push_back(static_cast<int>(list.tracks.size()));
                }
            }
        }

        // A grid-template-columns/-rows value; 'avail' is the length the
        // tracks share, for percentages and auto-fill.
        track_list parse_tracks(const css_token_vector& toks, pixel_t avail, pixel_t gap, int items,
                                const document::ptr& doc, const font_metrics& fm)
        {
            track_list list;
            for(const auto& tok : toks)
            {
                if(tok.type == SQUARE_BLOCK)
                {
                    add_names(list, tok);
                    continue;
                }
                if(tok.type == CV_FUNCTION && lowcase(tok.name()) == "repeat")
                {
                    // repeat(<count>, <tracks>): the count, then the rest of
                    // the tokens, which have no commas left in them
                    const auto& v = tok.value;
                    size_t      comma =
                        std::find_if(v.begin(), v.end(), [](const css_token& t) { return t.type == ','; }) - v.begin();
                    if(comma == 0 || comma >= v.size())
                    {
                        continue;
                    }
                    css_token_vector body(v.begin() + static_cast<long>(comma) + 1, v.end());
                    std::vector<track> one;
                    pixel_t            each = 0;
                    for(const auto& b : body)
                    {
                        track t;
                        if(b.type != SQUARE_BLOCK && parse_track(b, avail, doc, fm, t))
                        {
                            one.push_back(t);
                            each += t.min;
                        }
                    }
                    if(one.empty())
                    {
                        continue;
                    }
                    int         count = 1;
                    std::string mode  = v[0].ident();
                    if(mode == "auto-fill" || mode == "auto-fit")
                    {
                        // as many as fit, each at its least size; one at least
                        pixel_t step = each + gap * static_cast<int>(one.size());
                        if(step > 0_px)
                        {
                            count = std::max(1, static_cast<int>((avail + gap) / step));
                        }
                        if(mode == "auto-fit")
                        {
                            list.fit_from = static_cast<int>(list.tracks.size());
                            list.fit_to   = list.fit_from + count * static_cast<int>(one.size());
                        }
                    } else if(v[0].type == NUMBER)
                    {
                        count = std::clamp(static_cast<int>(v[0].n.number), 1, 1000);
                    }
                    for(int i = 0; i < count; i++)
                    {
                        for(const auto& b : body)
                        {
                            track t;
                            if(b.type == SQUARE_BLOCK)
                            {
                                add_names(list, b);
                            } else if(parse_track(b, avail, doc, fm, t))
                            {
                                list.tracks.push_back(t);
                            }
                        }
                    }
                    continue;
                }
                track t;
                if(parse_track(tok, avail, doc, fm, t))
                {
                    list.tracks.push_back(t);
                }
            }
            (void)items;
            return list;
        }

        struct area
        {
            int row0 = 1 << 30, row1 = -1, col0 = 1 << 30, col1 = -1; // tracks, inclusive
        };

        std::map<std::string, area> parse_areas(const css_token_vector& toks, int& rows, int& cols)
        {
            std::map<std::string, area> areas;
            rows = cols = 0;
            for(const auto& tok : toks)
            {
                if(tok.type != STRING)
                {
                    continue;
                }
                std::vector<std::string> cells;
                const std::string&       s = tok.str();
                for(size_t i = 0; i < s.size();)
                {
                    if(s[i] == ' ' || s[i] == '\t' || s[i] == '\n')
                    {
                        i++;
                        continue;
                    }
                    size_t j = i;
                    while(j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n')
                    {
                        j++;
                    }
                    cells.push_back(s.substr(i, j - i));
                    i = j;
                }
                for(int c = 0; c < static_cast<int>(cells.size()); c++)
                {
                    if(cells[c][0] == '.')
                    {
                        continue;
                    }
                    area& a = areas[cells[c]];
                    a.row0  = std::min(a.row0, rows);
                    a.row1  = std::max(a.row1, rows);
                    a.col0  = std::min(a.col0, c);
                    a.col1  = std::max(a.col1, c);
                }
                cols = std::max(cols, static_cast<int>(cells.size()));
                rows++;
            }
            return areas;
        }

        // A grid-*-start or -end: a line (0-based), a span, or auto
        struct line_ref
        {
            int  line = 0;
            int  span = 0; // > 0: span
            bool is_auto = true;
        };

        line_ref parse_line(const css_token_vector& toks, bool end, int count, const track_list& list,
                            const std::map<std::string, area>& areas, bool rows)
        {
            line_ref r;
            if(toks.empty() || toks[0].ident() == "auto")
            {
                return r;
            }
            bool        span = false;
            int         n    = 0;
            std::string name;
            for(const auto& t : toks)
            {
                if(t.ident() == "span")
                {
                    span = true;
                } else if(t.type == NUMBER)
                {
                    n = static_cast<int>(t.n.number);
                } else if(t.type == IDENT)
                {
                    name = t.name();
                }
            }
            if(span)
            {
                r.span = std::max(1, n);
                return r;
            }
            if(!name.empty())
            {
                // an area's name, or a line's, or "<area>-start" / "-end"
                auto lines = list.lines.find(name);
                if(lines != list.lines.end() && !lines->second.empty())
                {
                    int k     = n > 0 ? std::min(n, static_cast<int>(lines->second.size())) - 1 : 0;
                    r.line    = lines->second[k];
                    r.is_auto = false;
                    return r;
                }
                std::string base = name;
                bool        want_end = end;
                if(name.size() > 6 && name.compare(name.size() - 6, 6, "-start") == 0)
                {
                    base     = name.substr(0, name.size() - 6);
                    want_end = false;
                } else if(name.size() > 4 && name.compare(name.size() - 4, 4, "-end") == 0)
                {
                    base     = name.substr(0, name.size() - 4);
                    want_end = true;
                }
                auto a = areas.find(base);
                if(a != areas.end())
                {
                    int first = rows ? a->second.row0 : a->second.col0;
                    int last  = rows ? a->second.row1 : a->second.col1;
                    r.line    = want_end ? last + 1 : first;
                    r.is_auto = false;
                }
                return r;
            }
            if(n == 0)
            {
                return r;
            }
            // line numbers count from 1; negative ones from the end
            r.line    = n > 0 ? n - 1 : count + 1 + n;
            r.line    = std::max(0, r.line);
            r.is_auto = false;
            return r;
        }

        struct grid_item
        {
            std::shared_ptr<render_item> el;
            int                          row = -1, col = -1, rows = 1, cols = 1;
            rendered_width               want;
        };

        // start and end into a place and a span; -1: to be placed
        void resolve(const line_ref& s, const line_ref& e, int& at, int& span)
        {
            at   = -1;
            span = 1;
            if(!s.is_auto && !e.is_auto)
            {
                at   = std::min(s.line, e.line);
                span = std::max(1, std::abs(e.line - s.line));
            } else if(!s.is_auto)
            {
                at   = s.line;
                span = e.span > 0 ? e.span : 1;
            } else if(!e.is_auto)
            {
                span = s.span > 0 ? s.span : 1;
                at   = std::max(0, e.line - span);
            } else
            {
                span = s.span > 0 ? s.span : (e.span > 0 ? e.span : 1);
            }
        }

        class occupancy
        {
            std::vector<std::vector<bool>> m_used;
            int                            m_cols;

          public:
            explicit occupancy(int cols) :
                m_cols(cols)
            {
            }
            bool free(int r, int c, int rows, int cols)
            {
                if(c + cols > m_cols)
                {
                    return false;
                }
                for(int i = r; i < r + rows; i++)
                {
                    for(int j = c; j < c + cols; j++)
                    {
                        if(i < static_cast<int>(m_used.size()) && m_used[i][j])
                        {
                            return false;
                        }
                    }
                }
                return true;
            }
            void take(int r, int c, int rows, int cols)
            {
                while(static_cast<int>(m_used.size()) < r + rows)
                {
                    m_used.emplace_back(m_cols, false);
                }
                for(int i = r; i < r + rows; i++)
                {
                    for(int j = c; j < c + cols && j < m_cols; j++)
                    {
                        m_used[i][j] = true;
                    }
                }
            }
        };

        // Sizes the tracks along one axis. 'want' gives what each item
        // needs there: its least and its natural size, and which tracks it
        // spans. Shares 'avail' among them when it is known (>= 0).
        void size_tracks(std::vector<track>& tracks, pixel_t avail, pixel_t gap,
                         const std::vector<std::tuple<int, int, pixel_t, pixel_t>>& want)
        {
            int                  n = static_cast<int>(tracks.size());
            std::vector<pixel_t> need(n, 0), least(n, 0);
            for(const auto& [at, span, lo, hi] : want)
            {
                if(span == 1 && at < n)
                {
                    need[at]  = std::max(need[at], hi);
                    least[at] = std::max(least[at], lo);
                }
            }
            for(int i = 0; i < n; i++)
            {
                track& t = tracks[i];
                if(t.kind == track::fixed)
                {
                    t.size = t.min;
                } else if(t.kind == track::autos)
                {
                    t.size = std::max(t.min, need[i]);
                    if(t.max >= 0_px)
                    {
                        t.size = std::max(std::min(t.size, t.max), std::max(t.min, least[i]));
                    }
                } else
                {
                    t.size = std::max(t.min, least[i]);
                }
            }
            // an item spanning several tracks: the last auto one it spans
            // takes what they lack for it
            for(const auto& [at, span, lo, hi] : want)
            {
                if(span < 2 || at + span > n)
                {
                    continue;
                }
                pixel_t have = gap * (span - 1);
                int     last = -1;
                for(int i = at; i < at + span; i++)
                {
                    have += tracks[i].size;
                    if(tracks[i].kind == track::fr)
                    {
                        last = -2;
                        break;
                    }
                    if(tracks[i].kind == track::autos)
                    {
                        last = i;
                    }
                }
                if(last >= 0 && hi > have)
                {
                    tracks[last].size += hi - have;
                }
            }
            if(avail < 0_px)
            {
                return;
            }
            pixel_t used  = gap * std::max(0, n - 1);
            float   flex  = 0;
            int     autos = 0;
            for(const auto& t : tracks)
            {
                if(t.kind == track::fr)
                {
                    flex += t.flex;
                } else
                {
                    used += t.size;
                    autos += t.kind == track::autos;
                }
            }
            pixel_t left = avail - used;
            if(flex > 0)
            {
                // fr shares what is left; a track keeps what its items
                // need at least, and the others share less for that
                pixel_t unit = std::max(pixel_t(0), left) / std::max(1.0f, flex);
                for(int round = 0; round < 3; round++)
                {
                    pixel_t fixed_part = 0;
                    float   rest       = 0;
                    for(const auto& t : tracks)
                    {
                        if(t.kind == track::fr)
                        {
                            if(t.flex * unit < t.size && t.size > 0_px)
                            {
                                fixed_part += t.size;
                            } else
                            {
                                rest += t.flex;
                            }
                        }
                    }
                    if(rest <= 0)
                    {
                        break;
                    }
                    unit = std::max(pixel_t(0), left - fixed_part) / std::max(1.0f, rest);
                }
                for(auto& t : tracks)
                {
                    if(t.kind == track::fr)
                    {
                        t.size = std::max(t.size, unit * t.flex);
                    }
                }
            } else if(left > 0_px && autos > 0)
            {
                // no fr: auto tracks stretch to fill (justify-content: normal)
                for(auto& t : tracks)
                {
                    if(t.kind == track::autos)
                    {
                        t.size += left / autos;
                    }
                }
            } else if(left < 0_px && autos > 0)
            {
                // too wide: auto tracks give way, down to what their items
                // need at least
                pixel_t slack = 0;
                for(int i = 0; i < n; i++)
                {
                    if(tracks[i].kind == track::autos)
                    {
                        slack += std::max(pixel_t(0), tracks[i].size - least[i]);
                    }
                }
                if(slack > 0_px)
                {
                    pixel_t take = std::min(slack, -left);
                    for(int i = 0; i < n; i++)
                    {
                        if(tracks[i].kind == track::autos)
                        {
                            pixel_t s = std::max(pixel_t(0), tracks[i].size - least[i]);
                            tracks[i].size -= take * s / slack;
                        }
                    }
                }
            }
        }

        pixel_t offset_of(const std::vector<track>& tracks, pixel_t gap, int at)
        {
            pixel_t x = 0;
            for(int i = 0; i < at && i < static_cast<int>(tracks.size()); i++)
            {
                x += tracks[i].size + gap;
            }
            return x;
        }

        pixel_t span_of(const std::vector<track>& tracks, pixel_t gap, int at, int span)
        {
            pixel_t w = 0;
            for(int i = at; i < at + span && i < static_cast<int>(tracks.size()); i++)
            {
                w += tracks[i].size + (i > at ? gap : 0_px);
            }
            return w;
        }

        pixel_t gap_of(const css_length& g, pixel_t base)
        {
            return g.is_predefined() ? 0_px : g.calc_percent(base);
        }
    } // namespace

    rendered_width render_item_flex::_render_grid(pixel_t x, pixel_t y, const containing_block_context& self_size,
                                                  formatting_context* fmt_ctx)
    {
        const auto&   c     = css();
        document::ptr doc   = src_el()->get_document();
        const auto&   fm    = c.get_font_metrics();
        pixel_t       width = self_size.render_width;
        bool    fixed_height = self_size.height.type == containing_block_context::cbc_value_type_absolute;
        pixel_t height       = fixed_height ? pixel_t(self_size.height) - box_sizing_height() : -1_px;
        pixel_t col_gap      = gap_of(c.m_column_gap, width);
        pixel_t row_gap      = gap_of(c.m_row_gap, fixed_height ? height : 0_px);

        std::vector<grid_item> items;
        for(const auto& el : m_children)
        {
            auto pos = el->src_el()->css().get_position();
            if(pos == element_position_absolute || pos == element_position_fixed)
            {
                continue;
            }
            items.push_back({el});
        }
        std::stable_sort(items.begin(), items.end(), [](const grid_item& a, const grid_item& b) {
            return a.el->src_el()->css().get_order() < b.el->src_el()->css().get_order();
        });

        int  area_rows, area_cols;
        auto areas = parse_areas(c.m_grid_template_areas, area_rows, area_cols);
        auto cols  = parse_tracks(c.m_grid_template_columns, width, col_gap, static_cast<int>(items.size()), doc, fm);
        auto rows  = parse_tracks(c.m_grid_template_rows, fixed_height ? height : 0_px, row_gap, 0, doc, fm);
        track auto_col, auto_row;
        if(!c.m_grid_auto_columns.empty())
        {
            parse_track(c.m_grid_auto_columns[0], width, doc, fm, auto_col);
        }
        if(!c.m_grid_auto_rows.empty())
        {
            parse_track(c.m_grid_auto_rows[0], fixed_height ? height : 0_px, doc, fm, auto_row);
        }
        int ncols = std::max(static_cast<int>(cols.tracks.size()), area_cols);
        int nrows = std::max(static_cast<int>(rows.tracks.size()), area_rows);

        // where each item goes, by its lines; the columns it names widen
        // the grid before anything is placed automatically
        for(auto& it : items)
        {
            const auto& ic = it.el->src_el()->css();
            line_ref cs = parse_line(ic.m_grid_column_start, false, ncols, cols, areas, false);
            line_ref ce = parse_line(ic.m_grid_column_end, true, ncols, cols, areas, false);
            line_ref rs = parse_line(ic.m_grid_row_start, false, nrows, rows, areas, true);
            line_ref re = parse_line(ic.m_grid_row_end, true, nrows, rows, areas, true);
            resolve(cs, ce, it.col, it.cols);
            resolve(rs, re, it.row, it.rows);
            ncols = std::max(ncols, (it.col >= 0 ? it.col : 0) + it.cols);
        }
        ncols = std::max(ncols, 1);

        bool flow_column = false;
        for(const auto& t : c.m_grid_auto_flow)
        {
            flow_column |= t.ident() == "column";
        }
        occupancy used(ncols);
        for(auto& it : items)
        {
            if(it.row >= 0 && it.col >= 0)
            {
                used.take(it.row, it.col, it.rows, it.cols);
            }
        }
        int cur_row = 0, cur_col = 0;
        for(auto& it : items)
        {
            if(it.row >= 0 && it.col >= 0)
            {
                continue;
            }
            if(it.row >= 0)
            {
                // a row of its own: the first columns free in it
                int col = 0;
                while(!used.free(it.row, col, it.rows, it.cols) && col + it.cols <= ncols)
                {
                    col++;
                }
                it.col = col + it.cols <= ncols ? col : 0;
            } else if(it.col >= 0)
            {
                // a column of its own: the first row free there, from the
                // place auto placement has got to
                if(it.col < cur_col)
                {
                    cur_row++;
                }
                while(!used.free(cur_row, it.col, it.rows, it.cols))
                {
                    cur_row++;
                }
                it.row  = cur_row;
                cur_col = it.col;
            } else if(flow_column)
            {
                // column flow: down the column, then the next; the rows
                // are the explicit ones (one at least)
                int nr = std::max(1, nrows);
                while(true)
                {
                    if(cur_row + it.rows > nr)
                    {
                        cur_row = 0;
                        cur_col++;
                        if(cur_col + it.cols > ncols)
                        {
                            ncols = cur_col + it.cols;
                            used  = occupancy(ncols);
                            for(const auto& o : items)
                            {
                                if(o.row >= 0 && o.col >= 0 && &o != &it)
                                {
                                    used.take(o.row, o.col, o.rows, o.cols);
                                }
                            }
                        }
                    }
                    if(used.free(cur_row, cur_col, it.rows, it.cols))
                    {
                        break;
                    }
                    cur_row++;
                }
                it.row = cur_row;
                it.col = cur_col;
            } else
            {
                it.cols = std::min(it.cols, ncols);
                while(true)
                {
                    if(cur_col + it.cols > ncols)
                    {
                        cur_col = 0;
                        cur_row++;
                    }
                    if(used.free(cur_row, cur_col, it.rows, it.cols))
                    {
                        break;
                    }
                    cur_col++;
                }
                it.row = cur_row;
                it.col = cur_col;
            }
            used.take(it.row, it.col, it.rows, it.cols);
            if(!flow_column)
            {
                cur_col = it.col + it.cols;
            }
        }
        for(const auto& it : items)
        {
            nrows = std::max(nrows, it.row + it.rows);
        }

        // implicit tracks are grid-auto-*'s
        while(static_cast<int>(cols.tracks.size()) < ncols)
        {
            cols.tracks.push_back(auto_col);
        }
        while(static_cast<int>(rows.tracks.size()) < nrows)
        {
            rows.tracks.push_back(auto_row);
        }
        // auto-fit: the repeated tracks nothing is in take no room
        if(cols.fit_from >= 0)
        {
            std::vector<bool> taken(cols.tracks.size(), false);
            for(const auto& it : items)
            {
                for(int i = it.col; i < it.col + it.cols && i < static_cast<int>(taken.size()); i++)
                {
                    taken[i] = true;
                }
            }
            int last = static_cast<int>(cols.tracks.size());
            while(last > cols.fit_from && last > 1 && last - 1 < cols.fit_to && !taken[last - 1])
            {
                last--;
            }
            cols.tracks.resize(last);
            ncols = last;
            for(auto& it : items)
            {
                it.cols = std::min(it.cols, std::max(1, ncols - it.col));
            }
        }

        // the columns: by what the items want across
        std::vector<std::tuple<int, int, pixel_t, pixel_t>> want;
        for(auto& it : items)
        {
            it.want = it.el->render(0_px, 0_px, self_size.new_width(width, containing_block_context::size_mode_content),
                                    fmt_ctx);
            want.emplace_back(it.col, it.cols, it.want.min_width, it.want.natural_width);
        }
        size_tracks(cols.tracks, width, col_gap, want);

        // each item in its columns, to see how tall it is
        auto justify = [&](const grid_item& it) {
            int j = it.el->src_el()->css().m_justify_self;
            if(j == flex_align_items_auto)
            {
                j = c.m_justify_items;
            }
            return j;
        };
        for(auto& it : items)
        {
            pixel_t w  = span_of(cols.tracks, col_gap, it.col, it.cols);
            int     j  = justify(it);
            bool    st = j == flex_align_items_normal || j == flex_align_items_stretch;
            if(st && it.el->src_el()->css().get_width().is_predefined())
            {
                it.el->render(0_px, 0_px,
                              self_size.new_width(w - it.el->render_offset_width(),
                                                  containing_block_context::size_mode_exact_width),
                              fmt_ctx);
            } else
            {
                it.el->render(0_px, 0_px, self_size.new_width(std::min(w, it.want.natural_width)), fmt_ctx);
            }
        }

        // the rows: by how tall the items are
        want.clear();
        for(const auto& it : items)
        {
            want.emplace_back(it.row, it.rows, it.el->height(), it.el->height());
        }
        // with no height to share, fr rows are as tall as their items
        size_tracks(rows.tracks, fixed_height ? height : -1_px, row_gap, want);

        // and in place, aligned in their areas
        pixel_t total = 0;
        if(nrows > 0)
        {
            total = offset_of(rows.tracks, row_gap, nrows - 1) + rows.tracks[nrows - 1].size;
        }
        for(auto& it : items)
        {
            pixel_t cx = offset_of(cols.tracks, col_gap, it.col);
            pixel_t cy = offset_of(rows.tracks, row_gap, it.row);
            pixel_t cw = span_of(cols.tracks, col_gap, it.col, it.cols);
            pixel_t ch = span_of(rows.tracks, row_gap, it.row, it.rows);
            int     a  = it.el->src_el()->css().get_flex_align_self();
            if(a == flex_align_items_auto)
            {
                a = c.get_flex_align_items();
            }
            const auto& ic = it.el->src_el()->css();
            if((a == flex_align_items_normal || a == flex_align_items_stretch) && ic.get_height().is_predefined())
            {
                int  j  = justify(it);
                bool st = (j == flex_align_items_normal || j == flex_align_items_stretch) &&
                          ic.get_width().is_predefined();
                pixel_t w = st ? cw - it.el->render_offset_width() : it.el->pos().width + it.el->box_sizing_width();
                it.el->render(0_px, 0_px,
                              self_size.new_width_height(w, ch - it.el->render_offset_height(),
                                                         containing_block_context::size_mode_exact_width |
                                                             containing_block_context::size_mode_exact_height),
                              fmt_ctx);
            }
            pixel_t dx = 0, dy = 0;
            int     j  = justify(it);
            if(j == flex_align_items_center)
            {
                dx = (cw - it.el->width()) / 2;
            } else if(j == flex_align_items_end || j == flex_align_items_flex_end || j == flex_align_items_self_end)
            {
                dx = cw - it.el->width();
            }
            if(a == flex_align_items_center)
            {
                dy = (ch - it.el->height()) / 2;
            } else if(a == flex_align_items_end || a == flex_align_items_flex_end || a == flex_align_items_self_end)
            {
                dy = ch - it.el->height();
            }
            it.el->pos().x = cx + dx + it.el->content_offset_left();
            it.el->pos().y = cy + dy + it.el->content_offset_top();
        }

        m_pos.height = std::max(m_pos.height, total);
        m_pos.move_to(x, y);
        m_pos.x += content_offset_left();
        m_pos.y += content_offset_top();

        // as wide as the grid is at its widest and at its narrowest
        rendered_width ret;
        for(int i = 0; i < ncols; i++)
        {
            const auto& t = cols.tracks[i];
            pixel_t     hi = t.kind == track::fixed ? t.min : 0_px, lo = hi;
            for(const auto& it : items)
            {
                if(it.col == i && it.cols == 1 && t.kind != track::fixed)
                {
                    hi = std::max(hi, it.want.natural_width);
                    lo = std::max(lo, it.want.min_width);
                }
            }
            ret.natural_width += hi + (i ? col_gap : 0_px);
            ret.min_width += lo + (i ? col_gap : 0_px);
        }
        return ret;
    }
} // namespace litehtml
