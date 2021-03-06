/**
 * Copyright (c) 2007-2012, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file lnav.cc
 *
 * XXX This file has become a dumping ground for code and needs to be broken up
 * a bit.
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>

#include <math.h>
#include <time.h>
#include <glob.h>
#include <locale.h>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <readline/readline.h>

#include <map>
#include <set>
#include <stack>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <functional>

#include <sqlite3.h>

#ifdef HAVE_BZLIB_H
#include <bzlib.h>
#endif

#include "lnav.hh"
#include "help.hh"
#include "init-sql.hh"
#include "logfile.hh"
#include "lnav_log.hh"
#include "log_accel.hh"
#include "lnav_util.hh"
#include "ansi_scrubber.hh"
#include "listview_curses.hh"
#include "statusview_curses.hh"
#include "vt52_curses.hh"
#include "readline_curses.hh"
#include "textview_curses.hh"
#include "logfile_sub_source.hh"
#include "textfile_sub_source.hh"
#include "grep_proc.hh"
#include "bookmarks.hh"
#include "hist_source.hh"
#include "top_status_source.hh"
#include "bottom_status_source.hh"
#include "piper_proc.hh"
#include "log_vtab_impl.hh"
#include "db_sub_source.hh"
#include "pcrecpp.h"
#include "termios_guard.hh"
#include "data_parser.hh"
#include "xterm_mouse.hh"
#include "lnav_commands.hh"
#include "column_namer.hh"
#include "log_data_table.hh"
#include "log_format_loader.hh"
#include "session_data.hh"
#include "lnav_config.hh"
#include "sql_util.hh"
#include "sqlite-extension-func.h"
#include "sysclip.hh"
#include "term_extra.hh"
#include "log_data_helper.hh"
#include "readline_highlighters.hh"
#include "environ_vtab.hh"
#include "pretty_printer.hh"

#include "yajlpp.hh"

using namespace std;

static multimap<lnav_flags_t, string> DEFAULT_FILES;

struct _lnav_data lnav_data;

struct hist_level {
    int hl_bucket_size;
    int hl_group_size;
};

static struct hist_level HIST_ZOOM_VALUES[] = {
    { 24 * 60 * 60, 7 * 24 * 60 * 60 },
    {  4 * 60 * 60,     24 * 60 * 60 },
    {      60 * 60,     24 * 60 * 60 },
    {      10 * 60,          60 * 60 },
    {           60,          60 * 60 },
};

static const int HIST_ZOOM_LEVELS = sizeof(HIST_ZOOM_VALUES) /
                                    sizeof(struct hist_level);

static bookmark_type_t BM_EXAMPLE("");
static bookmark_type_t BM_QUERY("query");

const char *lnav_view_strings[LNV__MAX + 1] = {
    "log",
    "text",
    "help",
    "histogram",
    "graph",
    "db",
    "example",
    "schema",
    "pretty",

    NULL
};

static const char *view_titles[LNV__MAX] = {
    "LOG",
    "TEXT",
    "HELP",
    "HIST",
    "GRAPH",
    "DB",
    "EXAMPLE",
    "SCHEMA",
    "PRETTY",
};

static bool rescan_files(bool required = false);

class log_gutter_source : public list_gutter_source {
public:
    void listview_gutter_value_for_range(
        const listview_curses &lv, int start, int end, chtype &ch, int &fg_out) {
        textview_curses *tc = (textview_curses *)&lv;
        vis_bookmarks &bm = tc->get_bookmarks();
        vis_line_t next;
        bool search_hit = false;

        start -= 1;

        next = bm[&textview_curses::BM_SEARCH].next(vis_line_t(start));
        search_hit = (next != -1 && next <= end);

        next = bm[&BM_QUERY].next(vis_line_t(start));
        search_hit = search_hit || (next != -1 && next <= end);

        next = bm[&textview_curses::BM_USER].next(vis_line_t(start));
        if (next == -1) {
            next = bm[&textview_curses::BM_PARTITION].next(vis_line_t(start));
        }
        if (next != -1 && next <= end) {
            ch = search_hit ? ACS_PLUS : ACS_LTEE;
        }
        else {
            ch = search_hit ? ACS_RTEE : ACS_VLINE;
        }
        next = bm[&logfile_sub_source::BM_ERRORS].next(vis_line_t(start));
        if (next != -1 && next <= end) {
            fg_out = COLOR_RED;
        }
        else {
            next = bm[&logfile_sub_source::BM_WARNINGS].next(vis_line_t(start));
            if (next != -1 && next <= end) {
                fg_out = COLOR_YELLOW;
            }
        }
    };
};

class field_overlay_source : public list_overlay_source {
public:
    field_overlay_source(logfile_sub_source &lss)
        : fos_active(false),
          fos_active_prev(false),
          fos_log_helper(lss) {

    };

    size_t list_overlay_count(const listview_curses &lv)
    {
        logfile_sub_source &lss = lnav_data.ld_log_source;
        view_colors &vc = view_colors::singleton();

        if (!this->fos_active) {
            return 0;
        }

        if (lss.text_line_count() == 0) {
            this->fos_log_helper.clear();
            return 0;
        }

        content_line_t    cl   = lss.at(lv.get_top());

        if (!this->fos_log_helper.parse_line(cl)) {
            return 0;
        }

        this->fos_known_key_size = 0;
        this->fos_unknown_key_size = 0;

        for (std::vector<logline_value>::iterator iter =
                 this->fos_log_helper.ldh_line_values.begin();
             iter != this->fos_log_helper.ldh_line_values.end();
             ++iter) {
            this->fos_known_key_size = max(this->fos_known_key_size,
                                     (int)iter->lv_name.size());
        }

        for (data_parser::element_list_t::iterator iter =
                 this->fos_log_helper.ldh_parser->dp_pairs.begin();
             iter != this->fos_log_helper.ldh_parser->dp_pairs.end();
             ++iter) {
            std::string colname = this->fos_log_helper.ldh_parser->get_element_string(
                iter->e_sub_elements->front());

            colname = this->fos_log_helper.ldh_namer->add_column(colname);
            this->fos_unknown_key_size = max(
                this->fos_unknown_key_size, (int)colname.length());
        }

        this->fos_lines.clear();

        char old_timestamp[64], curr_timestamp[64];
        struct timeval curr_tv, offset_tv, orig_tv;
        char log_time[256];

        sql_strftime(curr_timestamp, sizeof(curr_timestamp),
           this->fos_log_helper.ldh_line->get_time(),
           this->fos_log_helper.ldh_line->get_millis(),
           'T');
        curr_tv = this->fos_log_helper.ldh_line->get_timeval();
        offset_tv = this->fos_log_helper.ldh_file->get_time_offset();
        timersub(&curr_tv, &offset_tv, &orig_tv);
        sql_strftime(old_timestamp, sizeof(old_timestamp),
           orig_tv.tv_sec, orig_tv.tv_usec / 1000,
           'T');
        snprintf(log_time, sizeof(log_time),
           " Current Time: %s  Original Time: %s  Offset: %+d.%03d",
           curr_timestamp,
           old_timestamp,
           (int)offset_tv.tv_sec, (int)(offset_tv.tv_usec / 1000));
        this->fos_lines.push_back(log_time);

        if (this->fos_log_helper.ldh_line_values.empty()) {
            this->fos_lines.push_back(" No known message fields");
        }
        else{
            this->fos_lines.push_back(" Known message fields:");
        }

        for (size_t lpc = 0; lpc < this->fos_log_helper.ldh_line_values.size(); lpc++) {
            logline_value &lv = this->fos_log_helper.ldh_line_values[lpc];
            attr_line_t al;
            string str;

            str = "   " + lv.lv_name.to_string();
            str.append(this->fos_known_key_size - lv.lv_name.size() + 3, ' ');
            str += " = " + lv.to_string();


            al.with_string(str)
              .with_attr(string_attr(
                line_range(3, 3 + lv.lv_name.size()),
                &view_curses::VC_STYLE,
                vc.attrs_for_ident(lv.lv_name.to_string())));

            this->fos_lines.push_back(al);
            this->add_key_line_attrs(this->fos_known_key_size);
        }

        std::map<const intern_string_t, json_ptr_walk::pair_list_t>::iterator json_iter;

        if (!this->fos_log_helper.ldh_json_pairs.empty()) {
            this->fos_lines.push_back(" JSON fields:");
        }

        for (json_iter = this->fos_log_helper.ldh_json_pairs.begin();
             json_iter != this->fos_log_helper.ldh_json_pairs.end();
             ++json_iter) {
            json_ptr_walk::pair_list_t &jpairs = json_iter->second;

            for (size_t lpc = 0; lpc < jpairs.size(); lpc++) {
                this->fos_lines.push_back("   " +
                    this->fos_log_helper.format_json_getter(json_iter->first, lpc) + " = " +
                    jpairs[lpc].second);
                this->add_key_line_attrs(0);
            }
        }

        if (this->fos_log_helper.ldh_parser->dp_pairs.empty()) {
            this->fos_lines.push_back(" No discovered message fields");
        }
        else {
            this->fos_lines.push_back(" Discovered message fields:");
        }

        data_parser::element_list_t::iterator iter;

        iter = this->fos_log_helper.ldh_parser->dp_pairs.begin();
        for (size_t lpc = 0;
             lpc < this->fos_log_helper.ldh_parser->dp_pairs.size(); lpc++, ++iter) {
            string &name = this->fos_log_helper.ldh_namer->cn_names[lpc];
            string val = this->fos_log_helper.ldh_parser->get_element_string(
                    iter->e_sub_elements->back());
            attr_line_t al("   " + name + " = " + val);

            al.with_attr(string_attr(
                line_range(3, 3 + name.length()),
                &view_curses::VC_STYLE,
                vc.attrs_for_ident(name)));

            this->fos_lines.push_back(al);
            this->add_key_line_attrs(this->fos_unknown_key_size,
                lpc == (this->fos_log_helper.ldh_parser->dp_pairs.size() - 1));
        }

        return this->fos_lines.size();
    };

    void add_key_line_attrs(int key_size, bool last_line = false) {
        string_attrs_t &sa = this->fos_lines.back().get_attrs();
        struct line_range lr(1, 2);
        sa.push_back(string_attr(lr, &view_curses::VC_GRAPHIC, last_line ? ACS_LLCORNER : ACS_LTEE));

        lr.lr_start = 3 + key_size + 3;
        lr.lr_end   = -1;
        sa.push_back(string_attr(lr, &view_curses::VC_STYLE, A_BOLD));
    };

    bool list_value_for_overlay(const listview_curses &lv,
                                vis_line_t y,
                                attr_line_t &value_out)
    {
        if (!this->fos_active || this->fos_log_helper.ldh_parser.get() == NULL) {
            return false;
        }

        int  row       = (int)y - 1;

        if (row < 0 || row >= (int)this->fos_lines.size()) {
            return false;
        }

        value_out = this->fos_lines[row];

        return true;
    };

    bool          fos_active;
    bool          fos_active_prev;
    log_data_helper fos_log_helper;
    int fos_known_key_size;
    int fos_unknown_key_size;
    vector<attr_line_t> fos_lines;
};

static int handle_collation_list(void *ptr,
                                 int ncols,
                                 char **colvalues,
                                 char **colnames)
{
    if (lnav_data.ld_rl_view != NULL) {
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", colvalues[1]);
    }

    return 0;
}

static int handle_db_list(void *ptr,
                          int ncols,
                          char **colvalues,
                          char **colnames)
{
    if (lnav_data.ld_rl_view != NULL) {
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", colvalues[1]);
    }

    return 0;
}

static int handle_table_list(void *ptr,
                             int ncols,
                             char **colvalues,
                             char **colnames)
{
    if (lnav_data.ld_rl_view != NULL) {
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", colvalues[0]);
    }

    return 0;
}

static int handle_table_info(void *ptr,
                             int ncols,
                             char **colvalues,
                             char **colnames)
{
    if (lnav_data.ld_rl_view != NULL) {
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", colvalues[1]);
    }
    if (strcmp(colvalues[5], "1") == 0) {
        lnav_data.ld_db_key_names.push_back(colvalues[1]);
    }
    return 0;
}

static int handle_foreign_key_list(void *ptr,
                                   int ncols,
                                   char **colvalues,
                                   char **colnames)
{
    lnav_data.ld_db_key_names.push_back(colvalues[3]);
    lnav_data.ld_db_key_names.push_back(colvalues[4]);
    return 0;
}

struct sqlite_metadata_callbacks lnav_sql_meta_callbacks = {
    handle_collation_list,
    handle_db_list,
    handle_table_list,
    handle_table_info,
    handle_foreign_key_list,
};

static void add_text_possibilities(
        int context, const string &type, const std::string &str)
{
    static pcrecpp::RE re_escape("([.\\^$*+?()\\[\\]{}\\\\|])");
    static pcrecpp::RE re_escape_no_dot("([\\^$*+?()\\[\\]{}\\\\|])");

    readline_curses *rlc = lnav_data.ld_rl_view;
    pcre_context_static<30> pc;
    data_scanner ds(str);
    data_token_t dt;

    while (ds.tokenize(pc, dt)) {
        if (pc[0]->length() < 4) {
            continue;
        }

        switch (dt) {
        case DT_DATE:
        case DT_TIME:
        case DT_WHITE:
            continue;
        default:
            break;
        }

        switch (context) {
        case LNM_SEARCH:
        case LNM_COMMAND: {
            string token_value, token_value_no_dot;

            token_value_no_dot = token_value =
                ds.get_input().get_substr(pc.all());
            re_escape.GlobalReplace("\\\\\\1", &token_value);
            re_escape_no_dot.GlobalReplace("\\\\\\1", &token_value_no_dot);
            rlc->add_possibility(context, type, token_value);
            if (token_value != token_value_no_dot) {
                rlc->add_possibility(context, type, token_value_no_dot);
            }
            break;
        }
        case LNM_SQL: {
            string token_value = ds.get_input().get_substr(pc.all());
            auto_mem<char, sqlite3_free> quoted_token;

            quoted_token = sqlite3_mprintf("%Q", token_value.c_str());
            rlc->add_possibility(context, type, std::string(quoted_token));
            break;
        }
        }

        switch (dt) {
        case DT_QUOTED_STRING:
            add_text_possibilities(context, type, ds.get_input().get_substr(pc[0]));
            break;
        default:
            break;
        }
    }
}

static void add_view_text_possibilities(
        int context, const string &type, textview_curses *tc)
{
    text_sub_source *tss = tc->get_sub_source();
    readline_curses *rlc = lnav_data.ld_rl_view;

    rlc->clear_possibilities(context, type);

    {
        auto_mem<FILE> pfile(pclose);

        pfile = open_clipboard(CT_FIND, CO_READ);
        if (pfile.in() != NULL) {
            char buffer[64];

            if (fgets(buffer, sizeof(buffer), pfile) != NULL) {
                char *nl;

                buffer[sizeof(buffer) - 1] = '\0';
                if ((nl = strchr(buffer, '\n')) != NULL) {
                    *nl = '\0';
                }
                rlc->add_possibility(context, type, std::string(buffer));
            }
        }
    }

    for (vis_line_t curr_line = tc->get_top();
         curr_line <= tc->get_bottom();
         ++curr_line) {
        string line;

        tss->text_value_for_line(*tc, curr_line, line);

        add_text_possibilities(context, type, line);
    }
}

static void add_env_possibilities(int context)
{
    extern char **environ;
    readline_curses *rlc = lnav_data.ld_rl_view;

    for (char **var = environ; *var != NULL; var++) {
        rlc->add_possibility(context, "*", "$" + string(*var, strchr(*var, '=')));
    }
}

static void add_filter_possibilities(textview_curses *tc)
{
    readline_curses *rc = lnav_data.ld_rl_view;
    text_sub_source *tss = tc->get_sub_source();
    filter_stack &fs = tss->get_filters();

    rc->clear_possibilities(LNM_COMMAND, "disabled-filter");
    rc->clear_possibilities(LNM_COMMAND, "enabled-filter");
    for (filter_stack::iterator iter = fs.begin();
            iter != fs.end();
            ++iter) {
        text_filter *tf = *iter;

        if (tf->is_enabled()) {
            rc->add_possibility(LNM_COMMAND, "enabled-filter", tf->get_id());
        }
        else {
            rc->add_possibility(LNM_COMMAND, "disabled-filter", tf->get_id());
        }
    }
}

static void add_mark_possibilities()
{
    readline_curses *rc = lnav_data.ld_rl_view;

    rc->clear_possibilities(LNM_COMMAND, "mark-type");
    for (bookmark_type_t::type_iterator iter = bookmark_type_t::type_begin();
         iter != bookmark_type_t::type_end();
         ++iter) {
        bookmark_type_t *bt = (*iter);

        if (bt->get_name().empty()) {
            continue;
        }
        rc->add_possibility(LNM_COMMAND, "mark-type", bt->get_name());
    }
}

bool setup_logline_table()
{
    // Hidden columns don't show up in the table_info pragma.
    static const char *hidden_table_columns[] = {
        "log_path",
        "log_text",

        NULL
    };

    static const char *commands[] = {
        ".schema",

        NULL
    };

    textview_curses &log_view = lnav_data.ld_views[LNV_LOG];
    bool             retval   = false;

    if (lnav_data.ld_rl_view != NULL) {
        lnav_data.ld_rl_view->clear_possibilities(LNM_SQL, "*");
        add_view_text_possibilities(LNM_SQL, "*", &log_view);
    }

    if (log_view.get_inner_height()) {
        vis_line_t     vl = log_view.get_top();
        content_line_t cl = lnav_data.ld_log_source.at_base(vl);

        lnav_data.ld_vtab_manager->unregister_vtab("logline");
        lnav_data.ld_vtab_manager->register_vtab(new log_data_table(cl));

        if (lnav_data.ld_rl_view != NULL) {
            log_data_helper ldh(lnav_data.ld_log_source);

            ldh.parse_line(cl);

            std::map<const intern_string_t, json_ptr_walk::pair_list_t>::const_iterator pair_iter;
            for (pair_iter = ldh.ldh_json_pairs.begin();
               pair_iter != ldh.ldh_json_pairs.end();
               ++pair_iter) {
                for (int lpc = 0; lpc < pair_iter->second.size(); lpc++) {
                    lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*",
                        ldh.format_json_getter(pair_iter->first, lpc));
                }
            }
        }

        retval = true;
    }

    lnav_data.ld_db_key_names.clear();

    if (lnav_data.ld_rl_view != NULL) {
        add_env_possibilities(LNM_SQL);

        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", sql_keywords);
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", sql_function_names);
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*",
            hidden_table_columns);
        lnav_data.ld_rl_view->add_possibility(LNM_SQL, "*", commands);

        for (int lpc = 0; sqlite_registration_funcs[lpc]; lpc++) {
            const struct FuncDef *basic_funcs;
            const struct FuncDefAgg *agg_funcs;

            sqlite_registration_funcs[lpc](&basic_funcs, &agg_funcs);
            for (int lpc2 = 0; basic_funcs && basic_funcs[lpc2].zName; lpc2++) {
                lnav_data.ld_rl_view->add_possibility(LNM_SQL,
                  "*",
                  basic_funcs[lpc2].zName);
            }
            for (int lpc2 = 0; agg_funcs && agg_funcs[lpc2].zName; lpc2++) {
                lnav_data.ld_rl_view->add_possibility(LNM_SQL,
                  "*",
                  agg_funcs[lpc2].zName);
            }
        }
    }

    walk_sqlite_metadata(lnav_data.ld_db.in(), lnav_sql_meta_callbacks);

    {
        log_vtab_manager::iterator iter;

        for (iter = lnav_data.ld_vtab_manager->begin();
             iter != lnav_data.ld_vtab_manager->end();
             ++iter) {
            iter->second->get_foreign_keys(lnav_data.ld_db_key_names);
        }
    }

    stable_sort(lnav_data.ld_db_key_names.begin(),
                lnav_data.ld_db_key_names.end());

    return retval;
}

/**
 * Observer for loading progress that updates the bottom status bar.
 */
class loading_observer
    : public logfile_observer {
public:
    loading_observer()
        : lo_last_offset(0) {

    };

    void logfile_indexing(logfile &lf, off_t off, size_t total)
    {
        static sig_atomic_t index_counter = 0;

        if (lnav_data.ld_flags & LNF_HEADLESS) {
            return;
        }

        /* XXX require(off <= total); */
        if (off > (off_t)total) {
            off = total;
        }

        if ((((size_t)off == total) && (this->lo_last_offset != off)) ||
            ui_periodic_timer::singleton().time_to_update(index_counter)) {
            lnav_data.ld_bottom_source.update_loading(off, total);
            this->do_update();
            this->lo_last_offset = off;
        }

        if (!lnav_data.ld_looping) {
            throw logfile::error(lf.get_filename(), EINTR);
        }
    };

private:
    void do_update(void)
    {
        lnav_data.ld_top_source.update_time();
        lnav_data.ld_status[LNS_TOP].do_update();
        lnav_data.ld_status[LNS_BOTTOM].do_update();
        refresh();
    };

    off_t          lo_last_offset;
};

static void rebuild_hist(size_t old_count, bool force)
{
    textview_curses &   hist_view = lnav_data.ld_views[LNV_HISTOGRAM];
    logfile_sub_source &lss       = lnav_data.ld_log_source;
    size_t       new_count        = lss.text_line_count();
    hist_source &hs         = lnav_data.ld_hist_source;
    int          zoom_level = lnav_data.ld_hist_zoom;
    time_t       old_time;
    int          lpc;

    old_time = hs.value_for_row(hist_view.get_top());
    hs.set_bucket_size(HIST_ZOOM_VALUES[zoom_level].hl_bucket_size);
    hs.set_group_size(HIST_ZOOM_VALUES[zoom_level].hl_group_size);
    if (force) {
        hs.clear();
        old_count = 0;
    }
    for (lpc = old_count; lpc < (int)new_count; lpc++) {
        logline *ll = lss.find_line(lss.at(vis_line_t(lpc)));

        if (!(ll->get_level() & logline::LEVEL_CONTINUED)) {
            hs.add_value(ll->get_time(),
                         bucket_type_t(ll->get_level() &
                                       ~logline::LEVEL__FLAGS));
        }
    }
    hist_view.reload_data();
    hist_view.set_top(hs.row_for_value(old_time));
}

class textfile_callback {
public:
    textfile_callback() : force(false), front_file(NULL), front_top(-1) { };

    void closed_file(logfile *lf) {
        lnav_data.ld_file_names.erase(make_pair(lf->get_filename(), lf->get_fd()));
        lnav_data.ld_files.remove(lf);
        delete lf;
    };

    void promote_file(logfile *lf) {
        if (lnav_data.ld_log_source.insert_file(lf)) {
            force = true;
        }
        else {
            this->closed_file(lf);
        }
    };

    void scanned_file(logfile *lf) {
        if (!lnav_data.ld_files_to_front.empty() &&
                lnav_data.ld_files_to_front.front().first ==
                        lf->get_filename()) {
            front_file = lf;
            front_top = lnav_data.ld_files_to_front.front().second;

            lnav_data.ld_files_to_front.pop_front();
        }
    };

    bool force;
    logfile *front_file;
    int front_top;
};

void rebuild_indexes(bool force)
{
    logfile_sub_source &lss       = lnav_data.ld_log_source;
    textview_curses &   log_view  = lnav_data.ld_views[LNV_LOG];
    textview_curses &   text_view = lnav_data.ld_views[LNV_TEXT];
    vis_line_t          old_bottom(0);
    content_line_t      top_content = content_line_t(-1);

    bool          scroll_down;
    size_t        old_count;
    time_t        old_time;

    old_count = lss.text_line_count();

    if (old_count) {
        top_content = lss.at(log_view.get_top());
    }

    {
        textfile_sub_source *          tss = &lnav_data.ld_text_source;
        std::list<logfile *>::iterator iter;
        bool new_data;

        old_bottom  = text_view.get_top_for_last_row();
        scroll_down = (text_view.get_top() >= old_bottom &&
            !(lnav_data.ld_flags & LNF_HEADLESS));

        textfile_callback cb;

        new_data = tss->rescan_files(cb);
        force = force || cb.force;

        if (cb.front_file != NULL) {
            ensure_view(&text_view);

            if (tss->current_file() != cb.front_file) {
                tss->to_front(cb.front_file);
                redo_search(LNV_TEXT);
                text_view.reload_data();
                old_bottom = vis_line_t(-1);

                new_data = false;
            }

            if (cb.front_top < 0) {
                cb.front_top += text_view.get_inner_height();
            }
            if (cb.front_top < text_view.get_inner_height()) {
                text_view.set_top(vis_line_t(cb.front_top));
                scroll_down = false;
            }
        }

        if (new_data && lnav_data.ld_search_child[LNV_TEXT].get() != NULL) {
            lnav_data.ld_search_child[LNV_TEXT]->get_grep_proc()->reset();
            lnav_data.ld_search_child[LNV_TEXT]->get_grep_proc()->
            queue_request(grep_line_t(-1));
            lnav_data.ld_search_child[LNV_TEXT]->get_grep_proc()->start();
        }
        text_view.reload_data();

        if (scroll_down && text_view.get_top_for_last_row() > text_view.get_top()) {
            text_view.set_top(text_view.get_top_for_last_row());
        }
    }

    old_time = lnav_data.ld_top_time;
    old_bottom  = log_view.get_top_for_last_row();
    scroll_down = (log_view.get_top() >= old_bottom &&
        !(lnav_data.ld_flags & LNF_HEADLESS));
    if (force) {
        old_count = 0;
    }

    list<logfile *>::iterator         file_iter;
    for (file_iter = lnav_data.ld_files.begin();
         file_iter != lnav_data.ld_files.end(); ) {
        logfile *lf = *file_iter;

        if (!lf->exists() || lf->is_closed()) {
            lnav_data.ld_file_names.erase(make_pair(lf->get_filename(), lf->get_fd()));
            lnav_data.ld_text_source.remove(lf);
            lnav_data.ld_log_source.remove_file(lf);
            file_iter = lnav_data.ld_files.erase(file_iter);
            force = true;

            delete lf;
        }
        else {
            ++file_iter;
        }
    }

    if (lss.rebuild_index(force)) {
        size_t      new_count = lss.text_line_count();
        grep_line_t start_line;
        int         lpc;

        log_view.reload_data();

        if (scroll_down && log_view.get_top_for_last_row() > log_view.get_top()) {
            log_view.set_top(log_view.get_top_for_last_row());
        }
        else if (!scroll_down && force) {
            content_line_t new_top_content = content_line_t(-1);

            if (new_count) {
                new_top_content = lss.at(log_view.get_top());
            }

            if (new_top_content != top_content) {
                log_view.set_top(lss.find_from_time(old_time));
            }
        }

        rebuild_hist(old_count, force);

        start_line = force ? grep_line_t(0) : grep_line_t(-1);

        if (force) {
            if (lnav_data.ld_search_child[LNV_LOG].get() != NULL) {
                lnav_data.ld_search_child[LNV_LOG]->get_grep_proc()->invalidate();
            }
            log_view.match_reset();
        }

        for (lpc = 0; lpc < LG__MAX; lpc++) {
            if (lnav_data.ld_grep_child[lpc].get() != NULL) {
                lnav_data.ld_grep_child[lpc]->get_grep_proc()->
                queue_request(start_line);
                lnav_data.ld_grep_child[lpc]->get_grep_proc()->start();
            }
        }
        if (lnav_data.ld_search_child[LNV_LOG].get() != NULL) {
            lnav_data.ld_search_child[LNV_LOG]->get_grep_proc()->reset();
            lnav_data.ld_search_child[LNV_LOG]->get_grep_proc()->
            queue_request(start_line);
            lnav_data.ld_search_child[LNV_LOG]->get_grep_proc()->start();
        }
    }

    if (!lnav_data.ld_view_stack.empty()) {
        textview_curses *tc = lnav_data.ld_view_stack.top();
        lnav_data.ld_bottom_source.update_filtered(tc->get_sub_source());
        lnav_data.ld_scroll_broadcaster.invoke(tc);
    }
}

class plain_text_source
    : public text_sub_source {
public:
    plain_text_source(string text)
    {
        size_t start = 0, end;

        while ((end = text.find('\n', start)) != string::npos) {
            this->tds_lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (start < text.length()) {
            this->tds_lines.push_back(text.substr(start));
        }
    };

    plain_text_source(const vector<string> &text_lines) {
        this->tds_lines = text_lines;
    };

    size_t text_line_count()
    {
        return this->tds_lines.size();
    };

    void text_value_for_line(textview_curses &tc,
                             int row,
                             string &value_out,
                             bool no_scrub)
    {
        value_out = this->tds_lines[row];
    };

    size_t text_size_for_line(textview_curses &tc, int row, bool raw) {
        return this->tds_lines[row].length();
    };

private:
    vector<string> tds_lines;
};

class time_label_source
    : public hist_source::label_source {
public:
    time_label_source() { };

    void hist_label_for_bucket(int bucket_start_value,
                               const hist_source::bucket_t &bucket,
                               string &label_out)
    {
        hist_source::bucket_t::const_iterator iter;
        int        total       = 0, errors = 0, warnings = 0;
        time_t     bucket_time = bucket_start_value;
        struct tm *bucket_tm;
        char       buffer[128];
        int        len;

        bucket_tm = gmtime((time_t *)&bucket_time);
        if (bucket_tm) {
            strftime(buffer, sizeof(buffer),
                     " %a %b %d %H:%M  ",
                     bucket_tm);
        }
        else {
            log_error("bad time %d", bucket_start_value);
            buffer[0] = '\0';
        }
        for (iter = bucket.begin(); iter != bucket.end(); iter++) {
            total += (int)iter->second;
            switch (iter->first) {
            case logline::LEVEL_FATAL:
            case logline::LEVEL_ERROR:
            case logline::LEVEL_CRITICAL:
                errors += (int)iter->second;
                break;

            case logline::LEVEL_WARNING:
                warnings += (int)iter->second;
                break;
            }
        }

        len = strlen(buffer);
        snprintf(&buffer[len], sizeof(buffer) - len,
                 " %8d total  %8d errors  %8d warnings",
                 total, errors, warnings);

        label_out = string(buffer);
    };
};

static bool append_default_files(lnav_flags_t flag)
{
    bool retval = true;

    if (lnav_data.ld_flags & flag) {
        pair<multimap<lnav_flags_t, string>::iterator,
             multimap<lnav_flags_t, string>::iterator> range;
        for (range = DEFAULT_FILES.equal_range(flag);
             range.first != range.second;
             range.first++) {
            string      path = range.first->second;
            struct stat st;

            if (access(path.c_str(), R_OK) == 0) {
                auto_mem<char> abspath;

                path = get_current_dir() + range.first->second;
                if ((abspath = realpath(path.c_str(), NULL)) == NULL) {
                    perror("Unable to resolve path");
                }
                else {
                    lnav_data.ld_file_names.insert(make_pair(abspath.in(),
                                                             -1));
                }
            }
            else if (stat(path.c_str(), &st) == 0) {
                fprintf(stderr,
                        "error: cannot read -- %s%s\n",
                        get_current_dir().c_str(),
                        path.c_str());
                retval = false;
            }
        }
    }

    return retval;
}

static void sigint(int sig)
{
    lnav_data.ld_looping = false;
}

static void sigwinch(int sig)
{
    lnav_data.ld_winched = true;
}

static void sigchld(int sig)
{
    lnav_data.ld_child_terminated = true;
}

static void back_ten(int ten_minute)
{
    textview_curses *   tc  = lnav_data.ld_view_stack.top();
    logfile_sub_source *lss;

    lss = dynamic_cast<logfile_sub_source *>(tc->get_sub_source());

    if (!lss)
        return;

    time_t hour = rounddown_offset(lnav_data.ld_top_time,
                                   60 * 60,
                                   ten_minute * 10 * 60);
    vis_line_t line = lss->find_from_time(hour);

    --line;
    lnav_data.ld_view_stack.top()->set_top(line);
}

static void update_view_name(void)
{
    status_field &sf = lnav_data.ld_top_source.statusview_value_for_field(
        top_status_source::TSF_VIEW_NAME);
    textview_curses * tc = lnav_data.ld_view_stack.top();
    struct line_range lr(0);

    sf.set_value("% 5s ", tc->get_title().c_str());
    sf.get_value().get_attrs().push_back(
        string_attr(lr, &view_curses::VC_STYLE,
            A_REVERSE | view_colors::ansi_color_pair(COLOR_BLUE, COLOR_WHITE)));
}

static void open_schema_view(void)
{
    textview_curses *schema_tc = &lnav_data.ld_views[LNV_SCHEMA];
    string schema;

    dump_sqlite_schema(lnav_data.ld_db, schema);

    schema += "\n\n-- Virtual Table Definitions --\n\n";
    schema += ENVIRON_CREATE_STMT;
    for (log_vtab_manager::iterator vtab_iter =
            lnav_data.ld_vtab_manager->begin();
         vtab_iter != lnav_data.ld_vtab_manager->end();
         ++vtab_iter) {
        schema += vtab_iter->second->get_table_statement();
    }

    if (schema_tc->get_sub_source() != NULL) {
        delete schema_tc->get_sub_source();
    }

    schema_tc->set_sub_source(new plain_text_source(schema));
}

static void open_pretty_view(void)
{
    textview_curses *log_tc = &lnav_data.ld_views[LNV_LOG];
    textview_curses *pretty_tc = &lnav_data.ld_views[LNV_PRETTY];
    logfile_sub_source &lss = lnav_data.ld_log_source;
    if (lss.text_line_count() > 0) {
        ostringstream stream;
        bool first_line = true;

        if (pretty_tc->get_sub_source() != NULL) {
            delete pretty_tc->get_sub_source();
        }
        for (vis_line_t vl = log_tc->get_top(); vl < log_tc->get_bottom(); ++vl) {
            content_line_t cl = lss.at(vl);
            logfile *lf = lss.find(cl);
            logfile::iterator ll = lf->begin() + cl;
            shared_buffer_ref sbr;

            if (!first_line && ll->is_continued()) {
                continue;
            }
            ll = lf->message_start(ll);

            lf->read_full_message(ll, sbr);
            data_scanner ds(sbr);
            pretty_printer pp(&ds);

            // TODO: dump more details of the line in the output.
            stream << pp.print() << endl;
            first_line = false;
        }
        pretty_tc->set_sub_source(new plain_text_source(stream.str()));
        if (lnav_data.ld_last_pretty_print_top != log_tc->get_top()) {
            pretty_tc->set_top(vis_line_t(0));
        }
        lnav_data.ld_last_pretty_print_top = log_tc->get_top();
    }
}

bool toggle_view(textview_curses *toggle_tc)
{
    textview_curses *tc     = lnav_data.ld_view_stack.empty() ? NULL : lnav_data.ld_view_stack.top();
    bool             retval = false;

    require(toggle_tc != NULL);
    require(toggle_tc >= &lnav_data.ld_views[0]);
    require(toggle_tc < &lnav_data.ld_views[LNV__MAX]);

    if (tc == toggle_tc) {
        lnav_data.ld_view_stack.pop();
    }
    else {
        if (toggle_tc == &lnav_data.ld_views[LNV_SCHEMA]) {
            open_schema_view();
        }
        else if (toggle_tc == &lnav_data.ld_views[LNV_PRETTY]) {
            open_pretty_view();
        }
        lnav_data.ld_view_stack.push(toggle_tc);
        retval = true;
    }
    tc = lnav_data.ld_view_stack.top();
    tc->set_needs_update();
    lnav_data.ld_scroll_broadcaster.invoke(tc);

    update_view_name();

    return retval;
}

void redo_search(lnav_view_t view_index)
{
    textview_curses *tc = &lnav_data.ld_views[view_index];

    tc->reload_data();
    if (lnav_data.ld_search_child[view_index].get() != NULL) {
        grep_proc *gp = lnav_data.ld_search_child[view_index]->get_grep_proc();

        tc->match_reset();
        gp->reset();
        gp->queue_request(grep_line_t(0));
        gp->start();
    }
    lnav_data.ld_scroll_broadcaster.invoke(tc);
}

/**
 * Ensure that the view is on the top of the view stack.
 *
 * @param expected_tc The text view that should be on top.
 * @return True if the view was already on the top of the stack.
 */
bool ensure_view(textview_curses *expected_tc)
{
    textview_curses *tc = lnav_data.ld_view_stack.empty() ? NULL : lnav_data.ld_view_stack.top();
    bool retval = true;

    if (tc != expected_tc) {
        toggle_view(expected_tc);
        retval = false;
    }
    return retval;
}

static vis_line_t next_cluster(
    vis_line_t(bookmark_vector<vis_line_t>::*f) (vis_line_t),
    bookmark_type_t *bt,
    vis_line_t top)
{
    textview_curses *tc = lnav_data.ld_view_stack.top();
    vis_bookmarks &bm = tc->get_bookmarks();
    bookmark_vector<vis_line_t> &bv = bm[bt];
    bool top_is_marked = binary_search(bv.begin(), bv.end(), top);
    vis_line_t last_top(top);

    while ((top = (bv.*f)(top)) != -1) {
        int diff = top - last_top;

        if (!top_is_marked || diff > 1) {
            return top;
        }
        else if (diff < -1) {
            last_top = top;
            while ((top = (bv.*f)(top)) != -1) {
                if (std::abs(last_top - top) > 1)
                    break;
                last_top = top;
            }
            return last_top;
        }
        last_top = top;
    }

    return vis_line_t(-1);
}

bool moveto_cluster(vis_line_t(bookmark_vector<vis_line_t>::*f) (vis_line_t),
        bookmark_type_t *bt,
        vis_line_t top)
{
    textview_curses *tc = lnav_data.ld_view_stack.top();
    vis_line_t new_top;

    new_top = next_cluster(f, bt, top);
    if (new_top != -1) {
        tc->set_top(new_top);
        return true;
    }

    alerter::singleton().chime();

    return false;
}

/* XXX For one, this code is kinda crappy.  For two, we should probably link
 * directly with X so we don't need to have xclip installed and it'll work if
 * we're ssh'd into a box.
 */
static void copy_to_xclip(void)
{
    textview_curses *tc = lnav_data.ld_view_stack.top();

    bookmark_vector<vis_line_t> &bv =
        tc->get_bookmarks()[&textview_curses::BM_USER];
    bookmark_vector<vis_line_t>::iterator iter;
    auto_mem<FILE> pfile(pclose);
    int    line_count = 0;
    string line;

    pfile = open_clipboard(CT_GENERAL);

    if (!pfile.in()) {
        flash();
        lnav_data.ld_rl_view->set_value(
            "error: Unable to copy to clipboard.  "
            "Make sure xclip or pbcopy is installed.");
        return;
    }

    for (iter = bv.begin(); iter != bv.end(); iter++) {
        tc->grep_value_for_line(*iter, line);
        fprintf(pfile, "%s\n", line.c_str());
        line_count += 1;
    }

    char buffer[128];

    snprintf(buffer, sizeof(buffer),
             "Copied " ANSI_BOLD("%d") " lines to the clipboard",
             line_count);
    lnav_data.ld_rl_view->set_value(buffer);
}

static void handle_paging_key(int ch)
{
    textview_curses *   tc  = lnav_data.ld_view_stack.top();
    logfile_sub_source *lss = NULL;
    vis_bookmarks &     bm  = tc->get_bookmarks();

    if (tc->handle_key(ch)) {
        return;
    }

    lss = dynamic_cast<logfile_sub_source *>(tc->get_sub_source());

    /* process the command keystroke */
    switch (ch) {
    case 'q':
    case 'Q':
    {
        string msg = "";

        if (tc == &lnav_data.ld_views[LNV_DB]) {
            msg = HELP_MSG_2(v, V, "to switch to the SQL result view");
        }
        else if (tc == &lnav_data.ld_views[LNV_HISTOGRAM]) {
            msg = HELP_MSG_2(i, I, "to switch to the histogram view");
        }
        else if (tc == &lnav_data.ld_views[LNV_TEXT]) {
            msg = HELP_MSG_1(t, "to switch to the text file view");
        }
        else if (tc == &lnav_data.ld_views[LNV_GRAPH]) {
            msg = HELP_MSG_1(g, "to switch to the graph view");
        }

        lnav_data.ld_rl_view->set_alt_value(msg);
    }
        lnav_data.ld_view_stack.pop();
        if (lnav_data.ld_view_stack.empty() ||
            (lnav_data.ld_view_stack.size() == 1 &&
             lnav_data.ld_log_source.text_line_count() == 0)) {
            lnav_data.ld_looping = false;
        }
        else {
            tc = lnav_data.ld_view_stack.top();
            tc->set_needs_update();
            lnav_data.ld_scroll_broadcaster.invoke(tc);
            update_view_name();
        }
        break;

    case KEY_F(2):
        if (xterm_mouse::is_available()) {
            lnav_data.ld_mouse.set_enabled(!lnav_data.ld_mouse.is_enabled());
        }
        else {
            lnav_data.ld_rl_view->set_value(
                "error: mouse support is not available, make sure your TERM is set to "
                "xterm or xterm-256color");
        }
        break;

    case 'c':
        copy_to_xclip();
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(
                C, "to clear marked messages"));
        break;

    case 'C':
        if (lss) {
            lss->text_clear_marks(&textview_curses::BM_USER);
        }

        lnav_data.ld_select_start.erase(tc);
        lnav_data.ld_last_user_mark.erase(tc);
        tc->get_bookmarks()[&textview_curses::BM_USER].clear();
        tc->reload_data();

        lnav_data.ld_rl_view->set_value("Cleared bookmarks");
        break;

    case 'e':
        moveto_cluster(&bookmark_vector<vis_line_t>::next,
                       &logfile_sub_source::BM_ERRORS,
                       tc->get_top());
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                w, W,
                                                "to move forward/backward through warning messages"));
        break;

    case 'E':
        moveto_cluster(&bookmark_vector<vis_line_t>::prev,
                       &logfile_sub_source::BM_ERRORS,
                       tc->get_top());
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                w, W,
                                                "to move forward/backward through warning messages"));
        break;

    case 'w':
        moveto_cluster(&bookmark_vector<vis_line_t>::next,
                       &logfile_sub_source::BM_WARNINGS,
                       tc->get_top());
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                o, O,
                                                "to move forward/backward an hour"));
        break;

    case 'W':
        moveto_cluster(&bookmark_vector<vis_line_t>::prev,
                       &logfile_sub_source::BM_WARNINGS,
                       tc->get_top());
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                o, O,
                                                "to move forward/backward an hour"));
        break;

    case 'n':
        tc->set_top(bm[&textview_curses::BM_SEARCH].next(tc->get_top()));
        lnav_data.ld_bottom_source.grep_error("");
        lnav_data.ld_rl_view->set_alt_value(
            "Press '" ANSI_BOLD(">") "' or '" ANSI_BOLD("<")
            "' to scroll horizontally to a search result");
        break;

    case 'N':
        tc->set_top(bm[&textview_curses::BM_SEARCH].prev(tc->get_top()));
        lnav_data.ld_bottom_source.grep_error("");
        lnav_data.ld_rl_view->set_alt_value(
            "Press '" ANSI_BOLD(">") "' or '" ANSI_BOLD("<")
            "' to scroll horizontally to a search result");
        break;

    case 'y':
        tc->set_top(bm[&BM_QUERY].next(tc->get_top()));
        break;

    case 'Y':
        tc->set_top(bm[&BM_QUERY].prev(tc->get_top()));
        break;

    case '>':
    {
        std::pair<int, int> range;

        tc->horiz_shift(tc->get_top(),
                        tc->get_bottom(),
                        tc->get_left(),
                        "$search",
                        range);
        if (range.second != INT_MAX) {
            tc->set_left(range.second);
            lnav_data.ld_rl_view->set_alt_value(
                HELP_MSG_1(m, "to bookmark a line"));
        }
        else{
            flash();
        }
    }
    break;

    case '<':
        if (tc->get_left() == 0) {
            flash();
        }
        else {
            std::pair<int, int> range;

            tc->horiz_shift(tc->get_top(),
                            tc->get_bottom(),
                            tc->get_left(),
                            "$search",
                            range);
            if (range.first != -1) {
                tc->set_left(range.first);
            }
            else{
                tc->set_left(0);
            }
            lnav_data.ld_rl_view->set_alt_value(
                HELP_MSG_1(m, "to bookmark a line"));
        }
        break;

    case 'f':
        if (tc == &lnav_data.ld_views[LNV_LOG]) {
            tc->set_top(bm[&logfile_sub_source::BM_FILES].next(tc->get_top()));
        }
        else if (tc == &lnav_data.ld_views[LNV_TEXT]) {
            textfile_sub_source &tss = lnav_data.ld_text_source;

            if (!tss.empty()) {
                tss.rotate_right();
                redo_search(LNV_TEXT);
            }
        }
        break;

    case 'F':
        if (tc == &lnav_data.ld_views[LNV_LOG]) {
            tc->set_top(bm[&logfile_sub_source::BM_FILES].prev(tc->get_top()));
        }
        else if (tc == &lnav_data.ld_views[LNV_TEXT]) {
            textfile_sub_source &tss = lnav_data.ld_text_source;

            if (!tss.empty()) {
                tss.rotate_left();
                redo_search(LNV_TEXT);
            }
        }
        break;

    case 'z':
        if (tc == &lnav_data.ld_views[LNV_HISTOGRAM]) {
            if ((lnav_data.ld_hist_zoom + 1) >= HIST_ZOOM_LEVELS) {
                flash();
            }
            else {
                lnav_data.ld_hist_zoom += 1;
                rebuild_hist(0, true);
            }

            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(
                                                    I,
                                                    "to switch to the log view at the top displayed time"));
        }
        break;

    case 'Z':
        if (tc == &lnav_data.ld_views[LNV_HISTOGRAM]) {
            if (lnav_data.ld_hist_zoom == 0) {
                flash();
            }
            else {
                lnav_data.ld_hist_zoom -= 1;
                rebuild_hist(0, true);
            }

            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(
                                                    I,
                                                    "to switch to the log view at the top displayed time"));
        }
        break;

    case 'u': {
        vis_line_t user_top, part_top;

        lnav_data.ld_rl_view->set_alt_value(
            HELP_MSG_1(c, "to copy marked lines to the clipboard; ")
            HELP_MSG_1(C, "to clear marked lines"));

        user_top = next_cluster(&bookmark_vector<vis_line_t>::next, 
            &textview_curses::BM_USER,
            tc->get_top());
        part_top = next_cluster(&bookmark_vector<vis_line_t>::next, 
            &textview_curses::BM_PARTITION,
            tc->get_top());
        if (part_top == -1 && user_top == -1) {
            alerter::singleton().chime();
        }
        else if (part_top == -1) {
            tc->set_top(user_top);
        }
        else if (user_top == -1) {
            tc->set_top(part_top);
        }
        else {
            tc->set_top(min(user_top, part_top));
        }
        break;
    }

    case 'U': {
        vis_line_t user_top, part_top;

        user_top = next_cluster(&bookmark_vector<vis_line_t>::prev, 
            &textview_curses::BM_USER,
            tc->get_top());
        part_top = next_cluster(&bookmark_vector<vis_line_t>::prev, 
            &textview_curses::BM_PARTITION,
            tc->get_top());
        if (part_top == -1 && user_top == -1) {
            alerter::singleton().chime();
        }
        else if (part_top == -1) {
            tc->set_top(user_top);
        }
        else if (user_top == -1) {
            tc->set_top(part_top);
        }
        else {
            tc->set_top(max(user_top, part_top));
        }
        break;
    }

    case 'm':
        lnav_data.ld_last_user_mark[tc] = tc->get_top();
        tc->toggle_user_mark(&textview_curses::BM_USER,
                             vis_line_t(lnav_data.ld_last_user_mark[tc]));
        tc->reload_data();

        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                            u, U,
                                            "to move forward/backward through user bookmarks"));
        break;

    case 'J':
        if (lnav_data.ld_last_user_mark.find(tc) ==
            lnav_data.ld_last_user_mark.end() ||
            !tc->is_visible(vis_line_t(lnav_data.ld_last_user_mark[tc]))) {
            lnav_data.ld_select_start[tc] = tc->get_top();
            lnav_data.ld_last_user_mark[tc] = tc->get_top();
        }
        else {
            vis_line_t    height;
            unsigned long width;

            tc->get_dimensions(height, width);
            if (lnav_data.ld_last_user_mark[tc] > tc->get_bottom() - 2 &&
                tc->get_top() + height < tc->get_inner_height()) {
                tc->shift_top(vis_line_t(1));
            }
            if (lnav_data.ld_last_user_mark[tc] + 1 >=
                tc->get_inner_height()) {
                break;
            }
            lnav_data.ld_last_user_mark[tc] += 1;
        }
        tc->toggle_user_mark(&textview_curses::BM_USER,
                             vis_line_t(lnav_data.ld_last_user_mark[tc]));
        tc->reload_data();

        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(
                                            c,
                                            "to copy marked lines to the clipboard"));
        break;

    case 'K':
        {
            int new_mark;

            if (lnav_data.ld_last_user_mark.find(tc) ==
                lnav_data.ld_last_user_mark.end() ||
                !tc->is_visible(vis_line_t(lnav_data.ld_last_user_mark[tc]))) {
                new_mark = tc->get_top();
            }
            else {
                new_mark = lnav_data.ld_last_user_mark[tc];
            }

            tc->toggle_user_mark(&textview_curses::BM_USER,
                                 vis_line_t(new_mark));
            if (new_mark == tc->get_top()) {
                tc->shift_top(vis_line_t(-1));
            }
            if (new_mark > 0) {
                lnav_data.ld_last_user_mark[tc] = new_mark - 1;
            }
            else {
                lnav_data.ld_last_user_mark[tc] = new_mark;
                flash();
            }
            lnav_data.ld_select_start[tc] = tc->get_top();
            tc->reload_data();

            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(
                                                    c,
                                                    "to copy marked lines to the clipboard"));
        }
        break;

    case 'L': {
        vis_line_t top = tc->get_top();
        vis_line_t bottom = tc->get_bottom();
        char line_break[80];

        nodelay(lnav_data.ld_window, 0);
        endwin();
        {
            guard_termios tguard(STDOUT_FILENO);
            struct termios new_termios = *tguard.get_termios();
            new_termios.c_oflag |= ONLCR | OPOST;
            tcsetattr(STDOUT_FILENO, TCSANOW, &new_termios);
            snprintf(line_break, sizeof(line_break),
                    "\n---------------- Lines %'d-%'d ----------------\n\n",
                    (int) top, (int) bottom);
            write(STDOUT_FILENO, line_break, strlen(line_break));
            for (; top <= bottom; ++top) {
                attr_line_t al;
                tc->listview_value_for_row(*tc, top, al);
                write(STDOUT_FILENO, al.get_string().c_str(), al.length());
                write(STDOUT_FILENO, "\n", 1);
            }
        }
        cbreak();
        getch();
        refresh();
        nodelay(lnav_data.ld_window, 1);
        break;
    }

    case 'M':
        if (lnav_data.ld_last_user_mark.find(tc) ==
            lnav_data.ld_last_user_mark.end()) {
            flash();
        }
        else {
            int start_line = min((int)tc->get_top(),
                                 lnav_data.ld_last_user_mark[tc] + 1);
            int end_line = max((int)tc->get_top(),
                               lnav_data.ld_last_user_mark[tc] - 1);

            tc->toggle_user_mark(&textview_curses::BM_USER,
                                  vis_line_t(start_line),
                                  vis_line_t(end_line));
            tc->reload_data();
        }
        break;

#if 0
    case 'S':
        {
            bookmark_vector<vis_line_t>::iterator iter;

            for (iter = bm[&textview_curses::BM_SEARCH].begin();
                 iter != bm[&textview_curses::BM_SEARCH].end();
                 ++iter) {
                tc->toggle_user_mark(&textview_curses::BM_USER, *iter);
            }

            lnav_data.ld_last_user_mark[tc] = -1;
            tc->reload_data();
        }
        break;
#endif

    case 's':
        if (lss) {
            vis_line_t next_top = vis_line_t(tc->get_top() + 2);

            if (!lss->is_time_offset_enabled()) {
                lnav_data.ld_rl_view->set_alt_value(
                    HELP_MSG_1(T, "to disable elapsed-time mode"));
            }
            lss->set_time_offset(true);
            while (next_top < tc->get_inner_height()) {
                if (lss->find_line(lss->at(next_top))->is_continued()) {
                }
                else if (lss->get_line_accel_direction(next_top) ==
                         log_accel::A_DECEL) {
                    --next_top;
                    tc->set_top(next_top);
                    break;
                }

                ++next_top;
            }
        }
        break;

    case 'S':
        if (lss) {
            vis_line_t next_top = tc->get_top();

            if (!lss->is_time_offset_enabled()) {
                lnav_data.ld_rl_view->set_alt_value(
                    HELP_MSG_1(T, "to disable elapsed-time mode"));
            }
            lss->set_time_offset(true);
            while (0 <= next_top && next_top < tc->get_inner_height()) {
                if (lss->find_line(lss->at(next_top))->is_continued()) {
                }
                else if (lss->get_line_accel_direction(next_top) ==
                         log_accel::A_DECEL) {
                    --next_top;
                    tc->set_top(next_top);
                    break;
                }

                --next_top;
            }
        }
        break;

    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
        if (lss) {
            int    ten_minute = (ch - '0') * 10 * 60;
            time_t hour       = rounddown(lnav_data.ld_top_time +
                                          (60 * 60) -
                                          ten_minute +
                                          1,
                                          60 * 60);
            vis_line_t line = lss->find_from_time(hour + ten_minute);

            tc->set_top(line);
        }
        break;

    case '!':
        back_ten(1);
        break;

    case '@':
        back_ten(2);
        break;

    case '#':
        back_ten(3);
        break;

    case '$':
        back_ten(4);
        break;

    case '%':
        back_ten(5);
        break;

    case '^':
        back_ten(6);
        break;

    case '9':
        if (lss) {
            double tenth = ((double)tc->get_inner_height()) / 10.0;

            tc->shift_top(vis_line_t(tenth));
        }
        break;

    case '(':
        if (lss) {
            double tenth = ((double)tc->get_inner_height()) / 10.0;

            tc->shift_top(vis_line_t(-tenth));
        }
        break;

    case '0':
        if (lss) {
            time_t     first_time = lnav_data.ld_top_time;
            int        step       = 24 * 60 * 60;
            vis_line_t line       =
                lss->find_from_time(roundup_size(first_time, step));

            tc->set_top(line);
        }
        break;

    case ')':
        if (lss) {
            time_t     day  = rounddown(lnav_data.ld_top_time, 24 * 60 * 60);
            vis_line_t line = lss->find_from_time(day);

            --line;
            tc->set_top(line);
        }
        break;

    case 'D':
    case 'O':
        if (tc->get_top() == 0) {
            flash();
        }
        else if (lss) {
            int        step     = ch == 'D' ? (24 * 60 * 60) : (60 * 60);
            time_t     top_time = lnav_data.ld_top_time;
            vis_line_t line     = lss->find_from_time(top_time - step);

            if (line != 0) {
                --line;
            }
            tc->set_top(line);

            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(/, "to search"));
        }
        break;

    case 'd':
    case 'o':
        if (lss) {
            int        step = ch == 'd' ? (24 * 60 * 60) : (60 * 60);
            vis_line_t line =
                lss->find_from_time(lnav_data.ld_top_time + step);

            tc->set_top(line);

            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_1(/, "to search"));
        }
        break;

    case ':':
        if (lnav_data.ld_views[LNV_LOG].get_inner_height() > 0) {
            logfile_sub_source &lss      = lnav_data.ld_log_source;
            textview_curses &   log_view = lnav_data.ld_views[LNV_LOG];
            content_line_t      cl       = lss.at(log_view.get_top());
            logfile *           lf       = lss.find(cl);
            logfile::iterator ll = lf->begin() + cl;
            log_data_helper ldh(lss);

            lnav_data.ld_rl_view->clear_possibilities(LNM_COMMAND, "colname");

            ldh.parse_line(log_view.get_top(), true);

            for (vector<string>::iterator iter = ldh.ldh_namer->cn_names.begin();
                 iter != ldh.ldh_namer->cn_names.end();
                 ++iter) {
                lnav_data.ld_rl_view->add_possibility(LNM_COMMAND, "colname", *iter);
            }

            ldh.clear();

            lnav_data.ld_rl_view->clear_possibilities(LNM_COMMAND, "line-time");
            {
                struct timeval tv = lf->get_time_offset();
                char buffer[64];

                sql_strftime(buffer, sizeof(buffer),
                             ll->get_time(), ll->get_millis(), 'T');
                lnav_data.ld_rl_view->add_possibility(LNM_COMMAND,
                                                      "line-time",
                                                      buffer);
                sql_strftime(buffer, sizeof(buffer),
                             ll->get_time() - tv.tv_sec,
                             ll->get_millis() - (tv.tv_usec / 1000),
                             'T');
                lnav_data.ld_rl_view->add_possibility(LNM_COMMAND,
                                                      "line-time",
                                                      buffer);
            }
        }

        add_view_text_possibilities(LNM_COMMAND, "filter", tc);
        lnav_data.ld_rl_view->
                add_possibility(LNM_COMMAND, "filter",
                lnav_data.ld_last_search[tc - lnav_data.ld_views]);
            add_filter_possibilities(tc);
            add_mark_possibilities();
        lnav_data.ld_mode = LNM_COMMAND;
        lnav_data.ld_rl_view->focus(LNM_COMMAND, ":");
        lnav_data.ld_bottom_source.set_prompt("Enter an lnav command: "
            "(Press " ANSI_BOLD("CTRL+]") " to abort)");
        break;

    case '/':
        lnav_data.ld_mode = LNM_SEARCH;
        lnav_data.ld_previous_search = lnav_data.ld_last_search[tc - lnav_data.ld_views];
        lnav_data.ld_search_start_line = tc->get_top();
        add_view_text_possibilities(LNM_SEARCH, "*", tc);
        lnav_data.ld_rl_view->focus(LNM_SEARCH, "/");
        lnav_data.ld_bottom_source.set_prompt(
            "Enter a regular expression to search for: "
            "(Press " ANSI_BOLD("CTRL+]") " to abort)");
        break;

    case ';':
        if (tc == &lnav_data.ld_views[LNV_LOG] ||
            tc == &lnav_data.ld_views[LNV_DB] ||
            tc == &lnav_data.ld_views[LNV_SCHEMA]) {
            textview_curses &log_view = lnav_data.ld_views[LNV_LOG];

            lnav_data.ld_mode = LNM_SQL;
            setup_logline_table();
            lnav_data.ld_rl_view->focus(LNM_SQL, ";");

            lnav_data.ld_bottom_source.update_loading(0, 0);
            lnav_data.ld_status[LNS_BOTTOM].do_update();

            field_overlay_source *fos;

            fos = (field_overlay_source *)log_view.get_overlay_source();
            fos->fos_active_prev = fos->fos_active;
            if (!fos->fos_active) {
                fos->fos_active = true;
                tc->reload_data();
            }
            lnav_data.ld_bottom_source.set_prompt("Enter an SQL query: (Press "
                ANSI_BOLD("CTRL+]") " to abort)");
        }
        break;

    case 'p':
        field_overlay_source *fos;

        fos =
            (field_overlay_source *)lnav_data.ld_views[LNV_LOG].
            get_overlay_source();
        fos->fos_active = !fos->fos_active;
        tc->reload_data();
        break;

    case 'P':
        if (tc == &lnav_data.ld_views[LNV_PRETTY] ||
                (lss && lss->text_line_count() > 0)) {
            toggle_view(&lnav_data.ld_views[LNV_PRETTY]);
        }
        else {
            lnav_data.ld_rl_view->set_value("Pretty-printed only works with log messages");
        }
        break;

    case 't':
        if (lnav_data.ld_text_source.current_file() == NULL) {
            flash();
            lnav_data.ld_rl_view->set_value("No text files loaded");
        }
        else if (toggle_view(&lnav_data.ld_views[LNV_TEXT])) {
            lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                    f, F,
                                                    "to switch to the next/previous file"));
        }
        break;

    case 'T':
        lnav_data.ld_log_source.toggle_time_offset();
        if (lss->is_time_offset_enabled()) {
            lnav_data.ld_rl_view->set_alt_value(
                HELP_MSG_2(s, S, "to move forward/backward through slow downs"));
        }
        tc->reload_data();
        break;

    case 'i':
        if (toggle_view(&lnav_data.ld_views[LNV_HISTOGRAM])) {
            lnav_data.ld_rl_view->set_alt_value(
                HELP_MSG_2(z, Z, "to zoom in/out"));
        }
        else {
            lnav_data.ld_rl_view->set_alt_value("");
        }
        break;

    case 'I':
    {
        time_t log_top = lnav_data.ld_top_time;

        if (toggle_view(&lnav_data.ld_views[LNV_HISTOGRAM])) {
            hist_source &hs = lnav_data.ld_hist_source;

            tc = lnav_data.ld_view_stack.top();
            tc->set_top(hs.row_for_value(log_top));
        }
        else {
            textview_curses &hist_tc = lnav_data.ld_views[LNV_HISTOGRAM];
            textview_curses &log_tc = lnav_data.ld_views[LNV_LOG];
            time_t hist_top =
                    lnav_data.ld_hist_source.value_for_row(hist_tc.get_top());
            lss = &lnav_data.ld_log_source;
            log_tc.set_top(lss->find_from_time(hist_top));
            log_tc.set_needs_update();
        }
    }
    break;

    case KEY_CTRL_G:
        toggle_view(&lnav_data.ld_views[LNV_GRAPH]);
        break;

    case '?':
        toggle_view(&lnav_data.ld_views[LNV_HELP]);
        break;

    case 'v':
        toggle_view(&lnav_data.ld_views[LNV_DB]);
        break;

    case 'V':
    {
        textview_curses *db_tc = &lnav_data.ld_views[LNV_DB];
        db_label_source &dls   = lnav_data.ld_db_rows;
        hist_source &    hs    = lnav_data.ld_db_source;

        if (toggle_view(db_tc)) {
            unsigned int lpc;

            for (lpc = 0; lpc < dls.dls_headers.size(); lpc++) {
                if (dls.dls_headers[lpc] != "log_line") {
                    continue;
                }

                char         linestr[64];
                int          line_number = (int)tc->get_top();
                unsigned int row;

                snprintf(linestr, sizeof(linestr), "%d", line_number);
                for (row = 0; row < dls.dls_rows.size(); row++) {
                    if (strcmp(dls.dls_rows[row][lpc],
                               linestr) == 0) {
                        vis_line_t db_line(hs.row_for_value(row));

                        db_tc->set_top(db_line);
                        db_tc->set_needs_update();
                        break;
                    }
                }
                break;
            }
        }
        else {
            int          db_row = hs.value_for_row(db_tc->get_top());
            unsigned int lpc;

            for (lpc = 0; lpc < dls.dls_headers.size(); lpc++) {
                if (dls.dls_headers[lpc] != "log_line") {
                    continue;
                }

                unsigned int line_number;

                tc = &lnav_data.ld_views[LNV_LOG];
                if (sscanf(dls.dls_rows[db_row][lpc],
                           "%d",
                           &line_number) &&
                    line_number < tc->listview_rows(*tc)) {
                    tc->set_top(vis_line_t(line_number));
                    tc->set_needs_update();
                }
                break;
            }
        }
    }
    break;

    case '\t':
    if (tc == &lnav_data.ld_views[LNV_DB])
    {
        hist_source &hs = lnav_data.ld_db_source;
        db_label_source &dls   = lnav_data.ld_db_rows;
        std::vector<bucket_type_t> &displayed = hs.get_displayed_buckets();
        std::vector<int>::iterator start_iter, iter;

        start_iter = dls.dls_headers_to_graph.begin();
        if (!displayed.empty()) {
            advance(start_iter, (int)displayed[0] + 1);
        }
        displayed.clear();
        iter = find(start_iter,
                    dls.dls_headers_to_graph.end(),
                    true);
        if (iter != dls.dls_headers_to_graph.end()) {
            bucket_type_t type;

            type = bucket_type_t(distance(dls.dls_headers_to_graph.begin(), iter));
            displayed.push_back(type);
        }
        if (displayed.empty()) {
            lnav_data.ld_rl_view->set_value("Graphing all values");
        }
        else {
            int index = displayed[0];

            lnav_data.ld_rl_view->set_value("Graphing column " ANSI_BOLD_START +
                dls.dls_headers[index] + ANSI_NORM);
        }
        tc->reload_data();
    }
    break;

    // XXX I'm sure there must be a better way to handle the difference between
    // iterator and reverse_iterator.
    case KEY_BTAB:
    if (tc == &lnav_data.ld_views[LNV_DB])
    {
        hist_source &hs = lnav_data.ld_db_source;
        db_label_source &dls   = lnav_data.ld_db_rows;
        std::vector<bucket_type_t> &displayed = hs.get_displayed_buckets();
        std::vector<int>::reverse_iterator start_iter, iter;

        start_iter = dls.dls_headers_to_graph.rbegin();
        if (!displayed.empty()) {
            advance(start_iter, dls.dls_headers_to_graph.size() - (int)displayed[0]);
        }
        displayed.clear();
        iter = find(start_iter,
                    dls.dls_headers_to_graph.rend(),
                    true);
        if (iter != dls.dls_headers_to_graph.rend()) {
            bucket_type_t type;

            type = bucket_type_t(distance(dls.dls_headers_to_graph.begin(), --iter.base()));
            displayed.push_back(type);
        }
        tc->reload_data();
    }
    break;

    case 'X':
        lnav_data.ld_rl_view->set_value(execute_command("close"));
        break;

    case '\\':
    {
        vis_bookmarks &bm = tc->get_bookmarks();
        string         ex;

        for (bookmark_vector<vis_line_t>::iterator iter =
                 bm[&BM_EXAMPLE].begin();
             iter != bm[&BM_EXAMPLE].end();
             ++iter) {
            string line;

            tc->get_sub_source()->text_value_for_line(*tc, *iter, line);
            ex += line + "\n";
        }
        lnav_data.ld_views[LNV_EXAMPLE].set_sub_source(new plain_text_source(
                                                           ex));
        ensure_view(&lnav_data.ld_views[LNV_EXAMPLE]);
    }
    break;

    case 'r':
        if (!lnav_data.ld_session_file_names.empty()) {
            lnav_data.ld_session_file_index =
                    (lnav_data.ld_session_file_index + 1) %
                            lnav_data.ld_session_file_names.size();
            reset_session();
            load_session();
            rebuild_indexes(true);
        }
        break;

    case 'R':
        if (lnav_data.ld_session_file_index == 0) {
            lnav_data.ld_session_file_index =
                lnav_data.ld_session_file_names.size() - 1;
        }
        else{
            lnav_data.ld_session_file_index -= 1;
        }
        reset_session();
        load_session();
        rebuild_indexes(true);
        break;

    case KEY_CTRL_R:
        reset_session();
        rebuild_indexes(true);
        lnav_data.ld_rl_view->set_alt_value(HELP_MSG_2(
                                                r, R,
                                                "to restore the next/previous session"));
        break;

    case KEY_CTRL_W:
        execute_command(lnav_data.ld_views[LNV_LOG].get_word_wrap() ?
            "disable-word-wrap" : "enable-word-wrap");
        break;

    case KEY_CTRL_L:
        execute_command("redraw");
        break;

    default:
        log_warning("unhandled %d", ch);
        lnav_data.ld_rl_view->set_value("Unrecognized keystroke, press "
                                        ANSI_BOLD("?")
                                        " to view help");
        flash();
        break;
    }
}

static void handle_rl_key(int ch)
{
    switch (ch) {
    case KEY_PPAGE:
    case KEY_NPAGE:
        handle_paging_key(ch);
        break;

    case KEY_CTRL_RBRACKET:
        lnav_data.ld_rl_view->abort();
        break;

    default:
        lnav_data.ld_rl_view->handle_key(ch);
        break;
    }
}

readline_context::command_map_t lnav_commands;

string execute_command(string cmdline)
{
    stringstream ss(cmdline);

    vector<string> args;
    string         buf, msg;

    log_info("Executing: %s", cmdline.c_str());

    while (ss >> buf) {
        args.push_back(buf);
    }

    if (args.size() > 0) {
        readline_context::command_map_t::iterator iter;

        if ((iter = lnav_commands.find(args[0])) ==
            lnav_commands.end()) {
            msg = "error: unknown command - " + args[0];
        }
        else {
            msg = iter->second(cmdline, args);
        }
    }

    return msg;
}

string execute_sql(string sql, string &alt_msg)
{
    db_label_source &      dls = lnav_data.ld_db_rows;
    hist_source &          hs  = lnav_data.ld_db_source;
    auto_mem<sqlite3_stmt> stmt(sqlite3_finalize);
    string stmt_str = trim(sql);
    string retval;
    int retcode;

    log_info("Executing SQL: %s", sql.c_str());

    lnav_data.ld_bottom_source.grep_error("");

    if (stmt_str == ".schema") {
        alt_msg = "";

        ensure_view(&lnav_data.ld_views[LNV_SCHEMA]);

        lnav_data.ld_mode = LNM_PAGING;
        return "";
    }

    hs.clear();
    hs.get_displayed_buckets().clear();
    dls.clear();
    dls.dls_stmt_str = stmt_str;
    retcode = sqlite3_prepare_v2(lnav_data.ld_db.in(),
       stmt_str.c_str(),
       -1,
       stmt.out(),
       NULL);
    if (retcode != SQLITE_OK) {
        const char *errmsg = sqlite3_errmsg(lnav_data.ld_db);

        retval = "error: " + string(errmsg);
        alt_msg = "";
    }
    else if (stmt == NULL) {
        retval = "";
        alt_msg = "";
    }
    else {
        bool done = false;
        int param_count;

        param_count = sqlite3_bind_parameter_count(stmt.in());
        for (int lpc = 0; lpc < param_count; lpc++) {
            const char *name;

            name = sqlite3_bind_parameter_name(stmt.in(), lpc + 1);
            if (name[0] == '$') {
                const char *env_value;

                if ((env_value = getenv(&name[1])) != NULL) {
                    sqlite3_bind_text(stmt.in(), lpc + 1, env_value, -1, SQLITE_STATIC);
                }
            }
        }

        if (lnav_data.ld_rl_view != NULL) {
            lnav_data.ld_rl_view->set_value("Executing query: " + sql + " ...");
            lnav_data.ld_rl_view->do_update();
        }

        lnav_data.ld_log_source.text_clear_marks(&BM_QUERY);
        while (!done) {
            retcode = sqlite3_step(stmt.in());

            switch (retcode) {
            case SQLITE_OK:
            case SQLITE_DONE:
                done = true;
                break;

            case SQLITE_ROW:
                sql_callback(stmt.in());
                break;

            default: {
                const char *errmsg;

                log_error("sqlite3_step error code: %d", retcode);
                errmsg = sqlite3_errmsg(lnav_data.ld_db);
                retval = "error: " + string(errmsg);
                done = true;
            }
                break;
            }
        }
    }

    if (retcode == SQLITE_DONE) {
        lnav_data.ld_views[LNV_LOG].reload_data();
        lnav_data.ld_views[LNV_DB].reload_data();
        lnav_data.ld_views[LNV_DB].set_left(0);

        if (dls.dls_rows.size() > 0) {
            vis_bookmarks &bm =
            lnav_data.ld_views[LNV_LOG].get_bookmarks();

            if (dls.dls_headers.size() == 1 && !bm[&BM_QUERY].empty()) {
                retval = "";
                alt_msg = HELP_MSG_2(
                  y, Y,
                  "to move forward/backward through query results "
                  "in the log view");
            }
            else if (dls.dls_rows.size() == 1) {
                string row;

                hs.text_value_for_line(lnav_data.ld_views[LNV_DB], 1, row, true);
                retval = "SQL Result: " + row;
            }
            else {
                char row_count[32];

                ensure_view(&lnav_data.ld_views[LNV_DB]);
                snprintf(row_count, sizeof(row_count),
                   ANSI_BOLD("%'d") " row(s) matched",
                   (int)dls.dls_rows.size());
                retval = row_count;
                alt_msg = HELP_MSG_2(
                  y, Y,
                  "to move forward/backward through query results "
                  "in the log view");
            }
        }
#ifdef HAVE_SQLITE3_STMT_READONLY
        else if (sqlite3_stmt_readonly(stmt.in())) {
            retval = "No rows matched";
            alt_msg = "";
        }
#endif
    }

    if (!(lnav_data.ld_flags & LNF_HEADLESS)) {
        lnav_data.ld_bottom_source.update_loading(0, 0);
        lnav_data.ld_status[LNS_BOTTOM].do_update();

        {
            field_overlay_source *fos;

            fos = (field_overlay_source *)lnav_data.ld_views[LNV_LOG].
                get_overlay_source();
            fos->fos_active = fos->fos_active_prev;

            redo_search(LNV_DB);
        }
    }
    lnav_data.ld_views[LNV_LOG].reload_data();

    return retval;
}

static void execute_file(string path)
{
    FILE *file;

    if (path == "-") {
        file = stdin;
    }
    else if ((file = fopen(path.c_str(), "r")) == NULL) {
        return;
    }

    int    line_number = 0;
    char *line = NULL;
    size_t line_max_size;
    ssize_t line_size;

    while ((line_size = getline(&line, &line_max_size, file)) != -1) {
        line_number += 1;

        if (trim(line).empty()) {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }

        string rc, alt_msg;

        if (line[line_size - 1] == '\n') {
            line[line_size - 1] = '\0';
        }
        switch (line[0]) {
        case ':':
            rc = execute_command(&line[1]);
            break;
        case '/':
        case ';':
            setup_logline_table();
            rc = execute_sql(&line[1], alt_msg);
            break;
        case '|':
            execute_file(&line[1]);
            break;
        default:
            rc = execute_command(line);
            break;
        }

        if (rescan_files()) {
            rebuild_indexes(true);
        }

        log_info("%s:%d:execute result -- %s",
            path.c_str(),
            line_number,
            rc.c_str());
    }

    if (file != stdin) {
        fclose(file);
    }
}

void execute_init_commands(vector<pair<string, string> > &msgs)
{
    if (lnav_data.ld_commands.empty()) {
        return;
    }

    for (std::list<string>::iterator iter = lnav_data.ld_commands.begin();
         iter != lnav_data.ld_commands.end();
         ++iter) {
        string msg, alt_msg;

        switch (iter->at(0)) {
        case ':':
            msg = execute_command(iter->substr(1));
            break;
        case '/':
        case ';':
            setup_logline_table();
            msg = execute_sql(iter->substr(1), alt_msg);
            break;
        case '|':
            execute_file(iter->substr(1));
            break;
        }

        msgs.push_back(make_pair(msg, alt_msg));

        if (rescan_files()) {
            rebuild_indexes(true);
        }
    }
    lnav_data.ld_commands.clear();
}

int sql_callback(sqlite3_stmt *stmt)
{
    logfile_sub_source &lss = lnav_data.ld_log_source;
    db_label_source &   dls = lnav_data.ld_db_rows;
    hist_source &       hs  = lnav_data.ld_db_source;
    int ncols = sqlite3_column_count(stmt);
    int row_number;
    int lpc, retval = 0;

    row_number = dls.dls_rows.size();
    dls.dls_rows.resize(row_number + 1);
    if (dls.dls_headers.empty()) {
        for (lpc = 0; lpc < ncols; lpc++) {
            int    type    = sqlite3_column_type(stmt, lpc);
            string colname = sqlite3_column_name(stmt, lpc);
            bool   graphable;

            graphable = ((type == SQLITE_INTEGER || type == SQLITE_FLOAT) &&
                         !binary_search(lnav_data.ld_db_key_names.begin(),
                                        lnav_data.ld_db_key_names.end(),
                                        colname));

            dls.push_header(colname, type, graphable);
            if (graphable) {
                hs.set_role_for_type(bucket_type_t(lpc),
                                     view_colors::singleton().
                                     next_plain_highlight());
            }
        }
    }
    for (lpc = 0; lpc < ncols; lpc++) {
        const char *value     = (const char *)sqlite3_column_text(stmt, lpc);
        double      num_value = 0.0;

        dls.push_column(value);
        if (dls.dls_headers[lpc] == "log_line") {
            int line_number = -1;

            if (sscanf(value, "%d", &line_number) == 1) {
                lss.text_mark(&BM_QUERY, line_number, true);
            }
        }
        if (dls.dls_headers_to_graph[lpc]) {
            sscanf(value, "%lf", &num_value);
            hs.add_value(row_number, bucket_type_t(lpc), num_value);
        }
        else {
            hs.add_empty_value(row_number);
        }
    }

    return retval;
}

void execute_search(lnav_view_t view, const std::string &regex_orig)
{
    auto_ptr<grep_highlighter> &gc = lnav_data.ld_search_child[view];
    textview_curses &           tc = lnav_data.ld_views[view];
    std::string regex = regex_orig;

    if ((gc.get() == NULL) || (regex != lnav_data.ld_last_search[view])) {
        const char *errptr;
        pcre *      code = NULL;
        int         eoff;
        bool quoted = false;

        tc.match_reset();

        if (regex.empty() && gc.get() != NULL) {
            tc.grep_begin(*(gc->get_grep_proc()));
            tc.grep_end(*(gc->get_grep_proc()));
        }
        gc.reset();

        log_debug("start search for: '%s'", regex.c_str());

        if (regex.empty()) {
            lnav_data.ld_bottom_source.grep_error("");
        }
        else if ((code = pcre_compile(regex.c_str(),
                                      PCRE_CASELESS,
                                      &errptr,
                                      &eoff,
                                      NULL)) == NULL) {
            lnav_data.ld_bottom_source.grep_error(
                "regexp error: " + string(errptr));

            quoted = true;
            regex = pcrecpp::RE::QuoteMeta(regex);

            log_info("invalid search regex, using quoted: %s", regex.c_str());
            if ((code = pcre_compile(regex.c_str(),
                                     PCRE_CASELESS,
                                     &errptr,
                                     &eoff,
                                     NULL)) == NULL) {
                log_error("Unable to compile quoted regex: %s", regex.c_str());
            }
        }

        if (code != NULL) {
            textview_curses::highlighter hl(
                code, false, view_colors::VCR_SEARCH);

            if (!quoted) {
                lnav_data.ld_bottom_source.grep_error("");
            }
            lnav_data.ld_bottom_source.set_prompt("");

            textview_curses::highlight_map_t &hm = tc.get_highlights();
            hm["$search"] = hl;

            auto_ptr<grep_proc> gp(new grep_proc(code,
               tc,
               lnav_data.ld_max_fd,
               lnav_data.ld_read_fds));

            gp->queue_request(grep_line_t(tc.get_top()));
            if (tc.get_top() > 0) {
                gp->queue_request(grep_line_t(0), grep_line_t(tc.get_top()));
            }
            gp->start();
            gp->set_sink(&tc);

            tc.set_follow_search(true);

            auto_ptr<grep_highlighter> gh(
                new grep_highlighter(gp, "$search", hm));
            gc = gh;
        }
    }

    lnav_data.ld_last_search[view] = regex;
}

static void rl_search_internal(void *dummy, readline_curses *rc, bool complete = false)
{
    string term_val;
    string name;

    switch (lnav_data.ld_mode) {
    case LNM_SEARCH:
        name = "$search";
        break;

    case LNM_CAPTURE:
        require(0);
        name = "$capture";
        break;

    case LNM_COMMAND:
        return;

    case LNM_SQL:
        term_val = trim(rc->get_value() + ";");

        if (term_val.size() > 0 && term_val[0] == '.') {
            lnav_data.ld_bottom_source.grep_error("");
        }
        else if (!sqlite3_complete(term_val.c_str())) {
            lnav_data.ld_bottom_source.
            grep_error("sql error: incomplete statement");
        }
        else {
            auto_mem<sqlite3_stmt> stmt(sqlite3_finalize);
            int retcode;

            retcode = sqlite3_prepare_v2(lnav_data.ld_db,
                                         rc->get_value().c_str(),
                                         -1,
                                         stmt.out(),
                                         NULL);
            if (retcode != SQLITE_OK) {
                const char *errmsg = sqlite3_errmsg(lnav_data.ld_db);

                lnav_data.ld_bottom_source.
                grep_error(string("sql error: ") + string(errmsg));
            }
            else {
                lnav_data.ld_bottom_source.grep_error("");
            }
        }
        return;

    default:
        require(0);
        break;
    }

    textview_curses *tc    = lnav_data.ld_view_stack.top();
    lnav_view_t      index = (lnav_view_t)(tc - lnav_data.ld_views);

    if (!complete) {
        tc->set_top(lnav_data.ld_search_start_line);
    }
    execute_search(index, rc->get_value());
}

static void rl_search(void *dummy, readline_curses *rc)
{
    rl_search_internal(dummy, rc);
}

static void rl_abort(void *dummy, readline_curses *rc)
{
    textview_curses *tc    = lnav_data.ld_view_stack.top();
    lnav_view_t      index = (lnav_view_t)(tc - lnav_data.ld_views);

    lnav_data.ld_bottom_source.set_prompt("");

    lnav_data.ld_bottom_source.grep_error("");
    switch (lnav_data.ld_mode) {
    case LNM_SEARCH:
        tc->set_top(lnav_data.ld_search_start_line);
        execute_search(index, lnav_data.ld_previous_search);
        break;
    case LNM_SQL:
    {
        field_overlay_source *fos;

        fos =
            (field_overlay_source *)lnav_data.ld_views[LNV_LOG].
            get_overlay_source();
        fos->fos_active = fos->fos_active_prev;
        tc->reload_data();
        break;
    }
    default:
        break;
    }
    lnav_data.ld_mode = LNM_PAGING;
}

static void rl_callback(void *dummy, readline_curses *rc)
{
    string alt_msg;

    lnav_data.ld_bottom_source.set_prompt("");
    switch (lnav_data.ld_mode) {
    case LNM_PAGING:
        require(0);
        break;

    case LNM_COMMAND:
        lnav_data.ld_mode = LNM_PAGING;
        rc->set_alt_value("");
        rc->set_value(execute_command(rc->get_value()));
        break;

    case LNM_SEARCH:
    case LNM_CAPTURE:
        rl_search_internal(dummy, rc, true);
        if (rc->get_value().size() > 0) {
            auto_mem<FILE> pfile(pclose);

            pfile = open_clipboard(CT_FIND);
            if (pfile.in() != NULL) {
                fprintf(pfile, "%s", rc->get_value().c_str());
            }
            lnav_data.ld_view_stack.top()->set_follow_search(false);
            rc->set_value("search: " + rc->get_value());
            rc->set_alt_value(HELP_MSG_2(
                                  n, N,
                                  "to move forward/backward through search results"));
        }
        lnav_data.ld_mode = LNM_PAGING;
        break;

    case LNM_SQL:
        rc->set_value(execute_sql(rc->get_value(), alt_msg));
        rc->set_alt_value(alt_msg);
        lnav_data.ld_mode = LNM_PAGING;
        break;
    }
}

static void rl_display_matches(void *dummy, readline_curses *rc)
{
    const std::vector<std::string> &matches = rc->get_matches();
    textview_curses &tc = lnav_data.ld_match_view;
    unsigned long width, height;
    int max_len, cols, rows, match_height, bottom_height;

    getmaxyx(lnav_data.ld_window, height, width);

    max_len = rc->get_max_match_length() + 2;
    cols = max(1UL, width / max_len);
    rows = (matches.size() + cols - 1) / cols;

    match_height = min((unsigned long)rows, (height - 4) / 2);
    bottom_height = match_height + 1 + rc->get_height();

    for (int lpc = 0; lpc < LNV__MAX; lpc++) {
        lnav_data.ld_views[lpc].set_height(vis_line_t(-bottom_height));
    }
    lnav_data.ld_status[LNS_BOTTOM].set_top(-bottom_height);

    if (tc.get_sub_source() != NULL) {
        delete tc.get_sub_source();
    }

    if (cols == 1) {
        tc.set_sub_source(new plain_text_source(rc->get_matches()));
    }
    else {
        std::vector<std::string> horiz_matches;

        horiz_matches.resize(rows);
        for (int lpc = 0; lpc < matches.size(); lpc++) {
            int curr_row = lpc % rows;

            horiz_matches[curr_row].append(matches[lpc]);
            horiz_matches[curr_row].append(
                max_len - matches[lpc].length(), ' ');
        }
        tc.set_sub_source(new plain_text_source(horiz_matches));
    }

    if (match_height > 0) {
        tc.set_window(lnav_data.ld_window);
        tc.set_y(height - bottom_height + 1);
        tc.set_height(vis_line_t(match_height));
        tc.reload_data();
    }
    else {
        tc.set_window(NULL);
    }
}

static void rl_display_next(void *dummy, readline_curses *rc)
{
    textview_curses &tc = lnav_data.ld_match_view;

    if (tc.get_top() >= (tc.get_top_for_last_row() - 1)) {
        tc.set_top(vis_line_t(0));
    }
    else {
        tc.shift_top(tc.get_height());
    }
}

static void usage(void)
{
    const char *usage_msg =
        "usage: %s [options] [logfile1 logfile2 ...]\n"
        "\n"
        "A curses-based log file viewer that indexes log messages by type\n"
        "and time to make it easier to navigate through files quickly.\n"
        "\n"
        "Key bindings:\n"
        "  ?     View/leave the online help text.\n"
        "  q     Quit the program.\n"
        "\n"
        "Options:\n"
        "  -h         Print this message, then exit.\n"
        "  -H         Display the internal help text.\n"
        "  -I path    An additional configuration directory.\n"
        "  -i         Install the given format files and exit.\n"
        "  -C         Check configuration and then exit.\n"
        "  -d file    Write debug messages to the given file.\n"
        "  -V         Print version information.\n"
        "\n"
        "  -a         Load all of the most recent log file types.\n"
        "  -r         Load older rotated log files as well.\n"
        "  -t         Prepend timestamps to the lines of data being read in\n"
        "             on the standard input.\n"
        "  -w file    Write the contents of the standard input to this file.\n"
        "\n"
        "  -c cmd     Execute a command after the files have been loaded.\n"
        "  -f path    Execute the commands in the given file.\n"
        "  -n         Run without the curses UI. (headless mode)\n"
        "  -q         Do not print the log messages after executing all\n"
        "             of the commands.\n"
        "\n"
        "Optional arguments:\n"
        "  logfile1          The log files or directories to view.  If a\n"
        "                    directory is given, all of the files in the\n"
        "                    directory will be loaded.\n"
        "\n"
        "Examples:\n"
        "  To load and follow the syslog file:\n"
        "    $ lnav\n"
        "\n"
        "  To load all of the files in /var/log:\n"
        "    $ lnav /var/log\n"
        "\n"
        "  To watch the output of make with timestamps prepended:\n"
        "    $ make 2>&1 | lnav -t\n"
        "\n"
        "Version: " PACKAGE_STRING "\n";

    fprintf(stderr, usage_msg, lnav_data.ld_program_name);
}

static pcre *xpcre_compile(const char *pattern, int options = 0)
{
    const char *errptr;
    pcre *      retval;
    int         eoff;

    if ((retval = pcre_compile(pattern,
                               options,
                               &errptr,
                               &eoff,
                               NULL)) == NULL) {
        fprintf(stderr, "internal error: failed to compile -- %s\n", pattern);
        fprintf(stderr, "internal error: %s\n", errptr);

        exit(1);
    }

    return retval;
}

/**
 * Callback used to keep track of the timestamps for the top and bottom lines
 * in the log view.  This function is intended to be used as the callback
 * function in a view_action.
 *
 * @param lv The listview object that contains the log
 */
static void update_times(void *, listview_curses *lv)
{
    if (lv == &lnav_data.ld_views[LNV_LOG] && lv->get_inner_height() > 0) {
        logfile_sub_source &lss = lnav_data.ld_log_source;
        logline *ll;

        ll = lss.find_line(lss.at(lv->get_top()));
        lnav_data.ld_top_time = ll->get_time();
        lnav_data.ld_top_time_millis = ll->get_millis();
        ll = lss.find_line(lss.at(lv->get_bottom()));
        lnav_data.ld_bottom_time = ll->get_time();
        lnav_data.ld_bottom_time_millis = ll->get_millis();
    }
    if (lv == &lnav_data.ld_views[LNV_HISTOGRAM] &&
        lv->get_inner_height() > 0) {
        hist_source &hs = lnav_data.ld_hist_source;

        lnav_data.ld_top_time    = hs.value_for_row(lv->get_top());
        lnav_data.ld_top_time_millis = 0;
        lnav_data.ld_bottom_time = hs.value_for_row(lv->get_bottom());
        lnav_data.ld_bottom_time_millis = 0;
    }
}

static void clear_last_user_mark(void *, listview_curses *lv)
{
    textview_curses *tc = (textview_curses *) lv;
    if (lnav_data.ld_select_start.find(tc) != lnav_data.ld_select_start.end() &&
            lnav_data.ld_select_start[tc] != tc->get_top()) {
        lnav_data.ld_select_start.erase(tc);
        lnav_data.ld_last_user_mark.erase(tc);
    }
}

/**
 * Functor used to compare files based on their device and inode number.
 */
struct same_file {
    same_file(const struct stat &stat) : sf_stat(stat) { };

    /**
     * Compare the given log file against the 'stat' given in the constructor.
     * @param  lf The log file to compare.
     * @return    True if the dev/inode values in the stat given in the
     *   constructor matches the stat in the logfile object.
     */
    bool operator()(const logfile *lf) const
    {
        return this->sf_stat.st_dev == lf->get_stat().st_dev &&
               this->sf_stat.st_ino == lf->get_stat().st_ino;
    };

    const struct stat &sf_stat;
};

/**
 * Try to load the given file as a log file.  If the file has not already been
 * loaded, it will be loaded.  If the file has already been loaded, the file
 * name will be updated.
 *
 * @param filename The file name to check.
 * @param fd       An already-opened descriptor for 'filename'.
 * @param required Specifies whether or not the file must exist and be valid.
 */
static void watch_logfile(string filename, int fd, bool required)
{
    static loading_observer obs;
    list<logfile *>::iterator file_iter;
    struct stat st;
    int         rc;

    if (lnav_data.ld_closed_files.count(filename)) {
        return;
    }

    if (fd != -1) {
        rc = fstat(fd, &st);
    }
    else {
        rc = stat(filename.c_str(), &st);
    }

    if (rc == 0) {
        if (!S_ISREG(st.st_mode)) {
            if (required) {
                rc    = -1;
                errno = EINVAL;
            }
            else {
                return;
            }
        }
    }
    if (rc == -1) {
        if (required) {
            throw logfile::error(filename, errno);
        }
        else{
            return;
        }
    }

    file_iter = find_if(lnav_data.ld_files.begin(),
                        lnav_data.ld_files.end(),
                        same_file(st));

    if (file_iter == lnav_data.ld_files.end()) {
        if (find(lnav_data.ld_other_files.begin(),
                 lnav_data.ld_other_files.end(),
                 filename) == lnav_data.ld_other_files.end()) {
            file_format_t ff = detect_file_format(filename);

            switch (ff) {
            case FF_SQLITE_DB:
                lnav_data.ld_other_files.push_back(filename);
                attach_sqlite_db(lnav_data.ld_db.in(), filename);
                break;

            default:
                /* It's a new file, load it in. */
                logfile *lf = new logfile(filename, fd);

                log_info("loading new file: %s", filename.c_str());
                    lf->set_logfile_observer(&obs);
                lnav_data.ld_files.push_back(lf);
                lnav_data.ld_text_source.push_back(lf);
                break;
            }
        }
    }
    else {
        /* The file is already loaded, but has been found under a different
         * name.  We just need to update the stored file name.
         */
        (*file_iter)->set_filename(filename);
    }
}

/**
 * Expand a glob pattern and call watch_logfile with the file names that match
 * the pattern.
 * @param path     The glob pattern to expand.
 * @param required Passed to watch_logfile.
 */
static void expand_filename(string path, bool required)
{
    static_root_mem<glob_t, globfree> gl;

    if (glob(path.c_str(), GLOB_NOCHECK, NULL, gl.inout()) == 0) {
        int lpc;

        if (gl->gl_pathc == 1 /*&& gl.gl_matchc == 0*/) {
            /* It's a pattern that doesn't match any files
             * yet, allow it through since we'll load it in
             * dynamically.
             */
            required = false;
        }
        if (gl->gl_pathc > 1 ||
            strcmp(path.c_str(), gl->gl_pathv[0]) != 0) {
            required = false;
        }
        for (lpc = 0; lpc < (int)gl->gl_pathc; lpc++) {
            auto_mem<char> abspath;

            if ((abspath = realpath(gl->gl_pathv[lpc], NULL)) == NULL) {
                if (required) {
                    fprintf(stderr, "Cannot find file: %s -- %s",
                        gl->gl_pathv[lpc], strerror(errno));
                }
            }
            else {
                watch_logfile(abspath.in(), -1, required);
            }
        }
    }
}

static bool rescan_files(bool required)
{
    set<pair<string, int> >::iterator iter;
    list<logfile *>::iterator         file_iter;
    bool retval = false;

    for (iter = lnav_data.ld_file_names.begin();
         iter != lnav_data.ld_file_names.end();
         iter++) {
        if (iter->second == -1) {
            expand_filename(iter->first, required);
            if (lnav_data.ld_flags & LNF_ROTATED) {
                string path = iter->first + ".*";

                expand_filename(path, false);
            }
        }
        else {
            watch_logfile(iter->first, iter->second, required);
        }
    }

    for (file_iter = lnav_data.ld_files.begin();
         file_iter != lnav_data.ld_files.end(); ) {
        logfile *lf = *file_iter;

        if (!lf->exists() || lf->is_closed()) {
            return true;
        }
        else {
            ++file_iter;
        }
    }

    return retval;
}

static string execute_action(log_data_helper &ldh,
                             int value_index,
                             const string &action_name)
{
    std::map<string, log_format::action_def>::const_iterator iter;
    logline_value &lv = ldh.ldh_line_values[value_index];
    logfile *lf = ldh.ldh_file;
    const log_format *format = lf->get_format();
    pid_t child_pid;
    string retval;

    iter = format->lf_action_defs.find(action_name);

    const log_format::action_def &action = iter->second;

    auto_pipe in_pipe(STDIN_FILENO);
    auto_pipe out_pipe(STDOUT_FILENO);
    auto_pipe err_pipe(STDERR_FILENO);

    in_pipe.open();
    if (action.ad_capture_output)
        out_pipe.open();
    err_pipe.open();

    child_pid = fork();

    in_pipe.after_fork(child_pid);
    out_pipe.after_fork(child_pid);
    err_pipe.after_fork(child_pid);

    switch (child_pid) {
    case -1:
        retval = "error: unable to fork child process -- " + string(strerror(errno));
        break;
    case 0: {
            const char *args[action.ad_cmdline.size() + 1];
            set<std::string> path_set(format->get_source_path());
            char env_buffer[64];
            int value_line;
            string path;

            setenv("LNAV_ACTION_FILE", lf->get_filename().c_str(), 1);
            snprintf(env_buffer, sizeof(env_buffer),
                "%ld",
                (ldh.ldh_line - lf->begin()) + 1);
            setenv("LNAV_ACTION_FILE_LINE", env_buffer, 1);
            snprintf(env_buffer, sizeof(env_buffer), "%d", ldh.ldh_y_offset + 1);
            setenv("LNAV_ACTION_MSG_LINE", env_buffer, 1);
            setenv("LNAV_ACTION_VALUE_NAME", lv.lv_name.get(), 1);
            value_line = ldh.ldh_y_offset - ldh.get_value_line(lv) + 1;
            snprintf(env_buffer, sizeof(env_buffer), "%d", value_line);
            setenv("LNAV_ACTION_VALUE_LINE", env_buffer, 1);

            for (set<string>::iterator path_iter = path_set.begin();
                 path_iter != path_set.end();
                 ++path_iter) {
                if (!path.empty()) {
                    path += ":";
                }
                path += *path_iter;
            }
            path += ":" + string(getenv("PATH"));
            setenv("PATH", path.c_str(), 1);
            for (size_t lpc = 0; lpc < action.ad_cmdline.size(); lpc++) {
                args[lpc] = action.ad_cmdline[lpc].c_str();
            }
            args[action.ad_cmdline.size()] = NULL;
            execvp(args[0], (char *const *) args);
            fprintf(stderr,
                "error: could not exec process -- %s:%s\n",
                args[0],
                strerror(errno));
            _exit(0);
        }
        break;
    default: {
            static int exec_count = 0;

            string value = lv.to_string();
            line_buffer lb;
            off_t off = 0;
            line_value lv;

            lnav_data.ld_children.push_back(child_pid);

            if (write(in_pipe.write_end(), value.c_str(), value.size()) == -1) {
                perror("execute_action write");
            }
            in_pipe.close();

            lb.set_fd(err_pipe.read_end());

            lb.read_line(off, lv);

            if (out_pipe.read_end() != -1) {
                piper_proc *pp = new piper_proc(out_pipe.read_end(), false);
                char desc[128];

                lnav_data.ld_pipers.push_back(pp);
                snprintf(desc,
                    sizeof(desc), "[%d] Output of %s",
                    exec_count++,
                    action.ad_cmdline[0].c_str());
                lnav_data.ld_file_names.insert(make_pair(
                    desc,
                    pp->get_fd()));
                lnav_data.ld_files_to_front.push_back(make_pair(desc, 0));
            }

            retval = string(lv.lv_start, lv.lv_len);
        }
        break;
    }

    return retval;
}

class action_delegate : public text_delegate {
public:
    action_delegate(logfile_sub_source &lss) : ad_log_helper(lss), ad_press_line(-1) { };

    virtual bool text_handle_mouse(textview_curses &tc, mouse_event &me) {
        bool retval = false;

        if (me.me_button != BUTTON_LEFT) {
            return false;
        }

        vis_line_t mouse_line = vis_line_t(tc.get_top() + me.me_y);
        int mouse_left = tc.get_left() + me.me_x;

        switch (me.me_state) {
        case BUTTON_STATE_PRESSED:
            if (mouse_line >= vis_line_t(0) && mouse_line <= tc.get_bottom()) {
                size_t line_end_index = 0;
                int x_offset;

                this->ad_press_line = mouse_line;
                this->ad_log_helper.parse_line(mouse_line, true);

                this->ad_log_helper.get_line_bounds(this->ad_line_index, line_end_index);

                struct line_range lr(this->ad_line_index, line_end_index);

                this->ad_press_value = -1;

                x_offset = this->ad_line_index + mouse_left;
                if (lr.contains(x_offset)) {
                    for (size_t lpc = 0;
                         lpc < this->ad_log_helper.ldh_line_values.size();
                         lpc++) {
                        logline_value &lv = this->ad_log_helper.ldh_line_values[lpc];

                        if (lv.lv_origin.contains(x_offset)) {
                            this->ad_press_value = lpc;
                            break;
                        }
                    }
                }
            }
            break;
        case BUTTON_STATE_DRAGGED:
            if (mouse_line != this->ad_press_line) {
                this->ad_press_value = -1;
            }
            if (this->ad_press_value != -1) {
                retval = true;
            }
            break;
        case BUTTON_STATE_RELEASED:
            if (this->ad_press_value != -1 && this->ad_press_line == mouse_line) {
                logline_value &lv = this->ad_log_helper.ldh_line_values[this->ad_press_value];
                int x_offset = this->ad_line_index + mouse_left;

                if (lv.lv_origin.contains(x_offset)) {
                    logfile *lf = this->ad_log_helper.ldh_file;
                    const vector<string> *actions;

                    actions = lf->get_format()->get_actions(lv);
                    if (actions != NULL && !actions->empty()) {
                        string rc = execute_action(
                            this->ad_log_helper, this->ad_press_value, actions->at(0));

                        lnav_data.ld_rl_view->set_value(rc);
                    }
                }
                retval = true;
            }
            break;
        }

        return retval;
    };

    log_data_helper ad_log_helper;
    vis_line_t ad_press_line;
    int ad_press_value;
    size_t ad_line_index;
};

class lnav_behavior : public mouse_behavior {
public:
    enum lb_mode_t {
        LB_MODE_NONE,
        LB_MODE_DOWN,
        LB_MODE_UP,
        LB_MODE_DRAG
    };

    lnav_behavior() {};

    int scroll_polarity(int button)
    {
        return button == xterm_mouse::XT_SCROLL_UP ? -1 : 1;
    };

    void mouse_event(int button, bool release, int x, int y)
    {
        textview_curses *   tc  = lnav_data.ld_view_stack.top();
        struct mouse_event me;

        switch (button & xterm_mouse::XT_BUTTON__MASK) {
        case xterm_mouse::XT_BUTTON1:
            me.me_button = BUTTON_LEFT;
            break;
        case xterm_mouse::XT_BUTTON2:
            me.me_button = BUTTON_MIDDLE;
            break;
        case xterm_mouse::XT_BUTTON3:
            me.me_button = BUTTON_RIGHT;
            break;
        case xterm_mouse::XT_SCROLL_UP:
            me.me_button = BUTTON_SCROLL_UP;
            break;
        case xterm_mouse::XT_SCROLL_DOWN:
            me.me_button = BUTTON_SCROLL_DOWN;
            break;
        }

        if (button & xterm_mouse::XT_DRAG_FLAG) {
            me.me_state = BUTTON_STATE_DRAGGED;
        }
        else if (release) {
            me.me_state = BUTTON_STATE_RELEASED;
        }
        else {
            me.me_state = BUTTON_STATE_PRESSED;
        }

        gettimeofday(&me.me_time, NULL);
        me.me_x = x - 1;
        me.me_y = y - tc->get_y() - 1;

        tc->handle_mouse(me);
    };

private:
};

static void handle_key(int ch)
{
    switch (ch) {
    case CEOF:
    case KEY_RESIZE:
        break;
    default:
        switch (lnav_data.ld_mode) {
        case LNM_PAGING:
            handle_paging_key(ch);
            break;

        case LNM_COMMAND:
        case LNM_SEARCH:
        case LNM_CAPTURE:
        case LNM_SQL:
            handle_rl_key(ch);
            break;

        default:
            require(0);
            break;
        }
    }
}

void update_hits(void *dummy, textview_curses *tc)
{
    if (!lnav_data.ld_view_stack.empty() &&
        tc == lnav_data.ld_view_stack.top()) {
        lnav_data.ld_bottom_source.update_hits(tc);
    }
}

static void gather_pipers(void)
{
    for (std::list<piper_proc *>::iterator iter = lnav_data.ld_pipers.begin();
         iter != lnav_data.ld_pipers.end(); ) {
        if ((*iter)->has_exited()) {
            delete *iter;
            iter = lnav_data.ld_pipers.erase(iter);
        } else {
            ++iter;
        }
    }
}

static void looper(void)
{
    try {
        readline_context command_context("cmd", &lnav_commands);

        readline_context search_context("search");
        readline_context index_context("capture");
        readline_context sql_context("sql", NULL, false);
        readline_curses  rlc;
        int lpc;

        command_context.set_highlighter(readline_command_highlighter);
        search_context
            .set_append_character(0)
            .set_highlighter(readline_regex_highlighter);
        sql_context.set_highlighter(readline_sqlite_highlighter);

        listview_curses::action::broadcaster &sb =
            lnav_data.ld_scroll_broadcaster;

        rlc.add_context(LNM_COMMAND, command_context);
        rlc.add_context(LNM_SEARCH, search_context);
        rlc.add_context(LNM_CAPTURE, index_context);
        rlc.add_context(LNM_SQL, sql_context);
        rlc.start();

        lnav_data.ld_rl_view = &rlc;

        lnav_data.ld_rl_view->add_possibility(
            LNM_COMMAND, "graph", "\\d+(?:\\.\\d+)?");
        lnav_data.ld_rl_view->add_possibility(
            LNM_COMMAND, "graph", "([:= \\t]\\d+(?:\\.\\d+)?)");

        lnav_data.ld_rl_view->add_possibility(
            LNM_COMMAND, "viewname", lnav_view_strings);

        lnav_data.ld_rl_view->add_possibility(
            LNM_COMMAND, "levelname", logline::level_names);

        (void)signal(SIGINT, sigint);
        (void)signal(SIGTERM, sigint);
        (void)signal(SIGWINCH, sigwinch);
        (void)signal(SIGCHLD, sigchld);
        (void)signal(SIGPIPE, SIG_IGN);

        screen_curses sc;
        lnav_behavior lb;

        ui_periodic_timer::singleton();

        lnav_data.ld_mouse.set_behavior(&lb);
        lnav_data.ld_mouse.set_enabled(check_experimental("mouse"));

        lnav_data.ld_window = sc.get_window();
        keypad(stdscr, TRUE);
        (void)nonl();
        (void)cbreak();
        (void)noecho();
        (void)nodelay(lnav_data.ld_window, 1);

        define_key("\033Od", KEY_BEG);
        define_key("\033Oc", KEY_END);

        view_colors::singleton().init();

        rlc.set_window(lnav_data.ld_window);
        rlc.set_y(-1);
        rlc.set_perform_action(readline_curses::action(rl_callback));
        rlc.set_timeout_action(readline_curses::action(rl_search));
        rlc.set_abort_action(readline_curses::action(rl_abort));
        rlc.set_display_match_action(
            readline_curses::action(rl_display_matches));
        rlc.set_display_next_action(
            readline_curses::action(rl_display_next));
        rlc.set_alt_value(HELP_MSG_2(
            e, E, "to move forward/backward through error messages"));

        (void)curs_set(0);

        lnav_data.ld_view_stack.push(&lnav_data.ld_views[LNV_LOG]);
        update_view_name();

        for (lpc = 0; lpc < LNV__MAX; lpc++) {
            lnav_data.ld_views[lpc].set_window(lnav_data.ld_window);
            lnav_data.ld_views[lpc].set_y(1);
            lnav_data.ld_views[lpc].
            set_height(vis_line_t(-(rlc.get_height() + 1)));
            lnav_data.ld_views[lpc].
            set_scroll_action(sb.get_functor());
            lnav_data.ld_views[lpc].set_search_action(
                textview_curses::action(update_hits));
        }

        lnav_data.ld_status[LNS_TOP].set_top(0);
        lnav_data.ld_status[LNS_BOTTOM].set_top(-(rlc.get_height() + 1));
        for (lpc = 0; lpc < LNS__MAX; lpc++) {
            lnav_data.ld_status[lpc].set_window(lnav_data.ld_window);
        }
        lnav_data.ld_status[LNS_TOP].set_data_source(
            &lnav_data.ld_top_source);
        lnav_data.ld_status[LNS_BOTTOM].set_data_source(
            &lnav_data.ld_bottom_source);

        lnav_data.ld_match_view.set_show_bottom_border(true);

        sb.push_back(view_action<listview_curses>(update_times));
        sb.push_back(view_action<listview_curses>(clear_last_user_mark));
        sb.push_back(&lnav_data.ld_top_source.filename_wire);
        sb.push_back(&lnav_data.ld_bottom_source.line_number_wire);
        sb.push_back(&lnav_data.ld_bottom_source.percent_wire);
        sb.push_back(&lnav_data.ld_bottom_source.marks_wire);
        sb.push_back(&lnav_data.ld_term_extra.filename_wire);

        FD_ZERO(&lnav_data.ld_read_fds);
        FD_SET(STDIN_FILENO, &lnav_data.ld_read_fds);
        lnav_data.ld_max_fd =
            max(STDIN_FILENO, rlc.update_fd_set(lnav_data.ld_read_fds));

        lnav_data.ld_status[0].window_change();
        lnav_data.ld_status[1].window_change();

        execute_file(dotlnav_path("session"));

        lnav_data.ld_scroll_broadcaster.invoke(lnav_data.ld_view_stack.top());

        bool session_loaded = false;
        ui_periodic_timer &timer = ui_periodic_timer::singleton();
        static sig_atomic_t index_counter;

        timer.start_fade(index_counter, 1);
        while (lnav_data.ld_looping) {
            fd_set         ready_rfds = lnav_data.ld_read_fds;
            struct timeval to = { 0, 333000 };
            int            rc;

            lnav_data.ld_top_source.update_time();

            if (rescan_files()) {
                rebuild_indexes(true);
            }

            lnav_data.ld_status[LNS_TOP].do_update();
            lnav_data.ld_view_stack.top()->do_update();
            lnav_data.ld_match_view.do_update();
            lnav_data.ld_status[LNS_BOTTOM].do_update();
            rlc.do_update();
            refresh();

            rc = select(lnav_data.ld_max_fd + 1,
                        &ready_rfds, NULL, NULL,
                        &to);

            if (rc < 0) {
                switch (errno) {
                case EBADF:
                {
                    int lpc, fd_flags;

                    log_error("bad file descriptor");
                    for (lpc = 0; lpc < FD_SETSIZE; lpc++) {
                        if (fcntl(lpc, F_GETFD, &fd_flags) == -1 &&
                            FD_ISSET(lpc, &lnav_data.ld_read_fds)) {
                            log_error("bad fd %d", lpc);
                        }
                    }
                    lnav_data.ld_looping = false;
                }
                break;
                case 0:
                case EINTR:
                    break;

                default:
                    log_error("select %s", strerror(errno));
                    lnav_data.ld_looping = false;
                    break;
                }
            }
            else {
                if (FD_ISSET(STDIN_FILENO, &ready_rfds)) {
                    static size_t escape_index = 0;
                    static char escape_buffer[32];

                    int ch;

                    while ((ch = getch()) != ERR) {
                        alerter::singleton().new_input(ch);

                        if (escape_index > sizeof(escape_buffer) - 1) {
                            escape_index = 0;
                        }
                        else if (escape_index > 0) {
                            escape_buffer[escape_index++] = ch;
                            escape_buffer[escape_index] = '\0';

                            if (strcmp("\x1b[", escape_buffer) == 0) {
                                lnav_data.ld_mouse.handle_mouse(ch);
                            }
                            else {
                                for (size_t lpc = 0; lpc < escape_index; lpc++) {
                                    handle_key(escape_buffer[lpc]);
                                }
                            }
                            escape_index = 0;
                            continue;
                        }
                        switch (ch) {
                        case CEOF:
                        case KEY_RESIZE:
                            break;

                        case '\x1b':
                            escape_index = 0;
                            escape_buffer[escape_index++] = ch;
                            escape_buffer[escape_index] = '\0';
                            break;

                        case KEY_MOUSE:
                            lnav_data.ld_mouse.handle_mouse(ch);
                            break;

                        default:
                            handle_key(ch);
                            break;
                        }

                        if (!lnav_data.ld_looping) {
                            // No reason to keep processing input after the
                            // user has quit.  The view stack will also be
                            // empty, which will cause issues.
                            break;
                        }
                    }
                }
                for (lpc = 0; lpc < LG__MAX; lpc++) {
                    auto_ptr<grep_highlighter> &gc =
                        lnav_data.ld_grep_child[lpc];

                    if (gc.get() != NULL) {
                        gc->get_grep_proc()->check_fd_set(ready_rfds);
                        if (lpc == LG_GRAPH) {
                            lnav_data.ld_views[LNV_GRAPH].reload_data();
                            /* XXX */
                        }
                    }
                }
                for (lpc = 0; lpc < LNV__MAX; lpc++) {
                    auto_ptr<grep_highlighter> &gc =
                        lnav_data.ld_search_child[lpc];

                    if (gc.get() != NULL) {
                        gc->get_grep_proc()->check_fd_set(ready_rfds);

                        if (!lnav_data.ld_view_stack.empty()) {
                            lnav_data.ld_bottom_source.
                            update_hits(lnav_data.ld_view_stack.top());
                        }
                    }
                }
                rlc.check_fd_set(ready_rfds);
            }

            if (timer.fade_diff(index_counter) == 0) {
                static bool initial_build = false;

                if (lnav_data.ld_mode == LNM_PAGING) {
                    timer.start_fade(index_counter, 1);
                }
                else {
                    timer.start_fade(index_counter, 3);
                }
                rebuild_indexes(false);
                if (!initial_build &&
                        lnav_data.ld_log_source.text_line_count() == 0 &&
                        lnav_data.ld_text_source.text_line_count() > 0) {
                    toggle_view(&lnav_data.ld_views[LNV_TEXT]);
                    lnav_data.ld_views[LNV_TEXT].set_top(vis_line_t(0));
                    lnav_data.ld_rl_view->set_alt_value(
                            HELP_MSG_2(f, F,
                                    "to switch to the next/previous file"));
                }
                if (!initial_build &&
                        lnav_data.ld_log_source.text_line_count() == 0 &&
                        !lnav_data.ld_other_files.empty()) {
                    ensure_view(&lnav_data.ld_views[LNV_SCHEMA]);
                }

                if (!initial_build && lnav_data.ld_flags & LNF_HELP) {
                    toggle_view(&lnav_data.ld_views[LNV_HELP]);
                    initial_build = true;
                }
                if (lnav_data.ld_log_source.text_line_count() > 0 ||
                        lnav_data.ld_text_source.text_line_count() > 0 ||
                        !lnav_data.ld_other_files.empty()) {
                    initial_build = true;
                }

                if (!session_loaded) {
                    load_session();
                    if (!lnav_data.ld_session_file_names.empty()) {
                        std::string ago;

                        ago = time_ago(lnav_data.ld_session_save_time);
                        lnav_data.ld_rl_view->set_value(
                                ("restored session from " ANSI_BOLD_START) +
                                        ago +
                                        (ANSI_NORM "; press Ctrl-R to reset session"));
                    }
                    rebuild_indexes(true);
                    session_loaded = true;
                }

                {
                    vector<pair<string, string> > msgs;

                    execute_init_commands(msgs);

                    if (!msgs.empty()) {
                        pair<string, string> last_msg = msgs.back();

                        lnav_data.ld_rl_view->set_value(last_msg.first);
                        lnav_data.ld_rl_view->set_alt_value(last_msg.second);
                    }
                }
            }

            if (lnav_data.ld_winched) {
                struct winsize size;

                lnav_data.ld_winched = false;

                if (ioctl(fileno(stdout), TIOCGWINSZ, &size) == 0) {
                    resizeterm(size.ws_row, size.ws_col);
                }
                rlc.window_change();
                lnav_data.ld_status[0].window_change();
                lnav_data.ld_status[1].window_change();
                lnav_data.ld_view_stack.top()->set_needs_update();
            }

            if (lnav_data.ld_child_terminated) {
                lnav_data.ld_child_terminated = false;

                for (std::list<pid_t>::iterator iter = lnav_data.ld_children.begin();
                    iter != lnav_data.ld_children.end();
                    ++iter) {
                    int rc, child_stat;

                    rc = waitpid(*iter, &child_stat, WNOHANG);
                    if (rc == -1 || rc == 0)
                        continue;

                    iter = lnav_data.ld_children.erase(iter);
                }

                gather_pipers();
            }
        }
    }
    catch (readline_curses::error & e) {
        log_error("error: %s", strerror(e.e_err));
    }
}

static void setup_highlights(textview_curses::highlight_map_t &hm)
{
    hm["$kw"] = textview_curses::highlighter(xpcre_compile(
        "(?:"
          "\\balter |"
          "\\band\\b|"
          "\\bas |"
          "\\bbetween\\b|"
          "\\bbool\\b|"
          "\\bboolean\\b|"
          "\\bbreak\\b|"
          "\\bcase\\b|"
          "\\bcatch\\b|"
          "\\bchar\\b|"
          "\\bclass\\b|"
          "\\bcollate\\b|"
          "\\bconst\\b|"
          "\\bcontinue\\b|"
          "\\bcreate\\s+(?:virtual)?|"
          "\\bdatetime\\b|"
          "\\bdef |"
          "\\bdefault[:\\s]|"
          "\\bdo\\b|"
          "\\bdone\\b|"
          "\\bdouble\\b|"
          "\\bdrop\\b|"
          "\\belif |"
          "\\belse\\b|"
          "\\benum\\b|"
          "\\bendif\\b|"
          "\\besac\\b|"
          "\\bexcept[\\s:]|"
          "\\bexists\\b|"
          "\\bexport\\b|"
          "\\bextends\\b|"
          "\\bextern\\b|"
          "\\bfalse\\b|"
          "\\bfi\\b|"
          "\\bfloat\\b|"
          "\\bfor\\b|"
          "\\bforeign\\s+key\\b|"
          "\\bfrom |"
          "\\bgoto\\b|"
          "\\bgroup by |"
          "\\bif\\b|"
          "\\bimport |"
          "\\bimplements\\b|"
          "\\bin\\b|"
          "\\binline\\b|"
          "\\binner\\b|"
          "\\binsert |"
          "\\bint\\b|"
          "\\binto\\b|"
          "\\binterface\\b|"
          "\\bjoin\\b|"
          "\\blambda\\b|"
          "\\blet\\b|"
          "\\blong\\b|"
          "\\bnamespace\\b|"
          "\\bnew\\b|"
          "\\bnot\\b|"
          "\\bnull\\b|"
          "\\boperator\\b|"
          "\\bor\\b|"
          "\\border by |"
          "\\bpackage\\b|"
          "\\bprimary\\s+key\\b|"
          "\\bprivate\\b|"
          "\\bprotected\\b|"
          "\\bpublic\\b|"
          "\\braise\\b|"
          "\\breferences\\b|"
          "\\b(?<!@)return\\b|"
          "\\bselect |"
          "\\bself\\b|"
          "\\bshift\\b|"
          "\\bshort\\b|"
          "\\bsizeof\\b|"
          "\\bstatic\\b|"
          "\\bstruct\\b|"
          "\\bswitch\\b|"
          "\\btable\\b|"
          "\\btemplate\\b|"
          "\\bthen\\b|"
          "\\bthis\\b|"
          "\\b(?<!@)throws?\\b|"
          "\\btrue\\b|"
          "\\btry\\b|"
          "\\btypedef |"
          "\\btypename |"
          "\\bunion\\b|"
          "\\bunsigned |"
          "\\bupdate |"
          "\\busing |"
          "\\bvar\\b|"
          "\\bview\\b|"
          "\\bvoid\\b|"
          "\\bvolatile\\b|"
          "\\bwhere |"
          "\\bwhile\\b|"
          "\\b[a-zA-Z][\\w]+_t\\b"
          ")", PCRE_CASELESS),
        false, view_colors::VCR_KEYWORD);
    hm["$srcfile"] = textview_curses::
                     highlighter(xpcre_compile(
                                     "[\\w\\-_]+\\."
                                     "(?:java|a|o|so|c|cc|cpp|cxx|h|hh|hpp|hxx|py|pyc|rb):"
                                     "\\d+"));
    hm["$xml"] = textview_curses::
                 highlighter(xpcre_compile("<(/?[^ >=]+)[^>]*>"));
    hm["$stringd"] = textview_curses::
                     highlighter(xpcre_compile("\"(?:\\\\.|[^\"])*\""),
                                 false, view_colors::VCR_STRING);
    hm["$strings"] = textview_curses::
                     highlighter(xpcre_compile(
                                     "(?<![A-WY-Za-qstv-z])\'(?:\\\\.|[^'])*\'"),
                     false, view_colors::VCR_STRING);
    hm["$stringb"] = textview_curses::
                     highlighter(xpcre_compile("`(?:\\\\.|[^`])*`"),
                                 false, view_colors::VCR_STRING);
    hm["$diffp"] = textview_curses::
                   highlighter(xpcre_compile(
                                   "^\\+.*"), false,
                               view_colors::VCR_DIFF_ADD);
    hm["$diffm"] = textview_curses::
                   highlighter(xpcre_compile(
                                   "^(?:--- .*|-$|-[^-].*)"), false,
                               view_colors::VCR_DIFF_DELETE);
    hm["$diffs"] = textview_curses::
                   highlighter(xpcre_compile(
                                   "^\\@@ .*"), false,
                               view_colors::VCR_DIFF_SECTION);
    hm["$ip"] = textview_curses::
                highlighter(xpcre_compile("\\d+\\.\\d+\\.\\d+\\.\\d+"));
    hm["$comment"] = textview_curses::highlighter(xpcre_compile(
        "(?<=[\\s;])//.*|/\\*.*\\*/|\\(\\*.*\\*\\)|^#.*|\\s+#.*|dnl.*"), false, view_colors::VCR_COMMENT);
    hm["$javadoc"] = textview_curses::highlighter(xpcre_compile(
        "@(?:author|deprecated|exception|file|param|return|see|since|throws|todo|version)"));
    hm["$var"] = textview_curses::highlighter(xpcre_compile(
        "(?:"
          "(?:var\\s+)?([\\-\\w]+)\\s*=|"
          "(?<!\\$)\\$(\\w+)|"
          "(?<!\\$)\\$\\((\\w+)\\)|"
          "(?<!\\$)\\$\\{(\\w+)\\}"
          ")"),
        false, view_colors::VCR_VARIABLE);
}

int sql_progress(const struct log_cursor &lc)
{
    static sig_atomic_t sql_counter = 0;

    size_t total = lnav_data.ld_log_source.text_line_count();
    off_t  off   = lc.lc_curr_line;

    if (lnav_data.ld_window == NULL) {
        return 0;
    }

    if (!lnav_data.ld_looping) {
        return 1;
    }

    if (ui_periodic_timer::singleton().time_to_update(sql_counter)) {
        lnav_data.ld_bottom_source.update_loading(off, total);
        lnav_data.ld_top_source.update_time();
        lnav_data.ld_status[LNS_TOP].do_update();
        lnav_data.ld_status[LNS_BOTTOM].do_update();
        refresh();
    }

    return 0;
}

static void print_errors(vector<string> error_list)
{
    for (std::vector<std::string>::iterator iter = error_list.begin();
         iter != error_list.end();
         ++iter) {
        fprintf(stderr, "%s%s", iter->c_str(),
                (*iter)[iter->size() - 1] == '\n' ? "" : "\n");
    }
}

int main(int argc, char *argv[])
{
    std::vector<std::string> loader_errors;
    int lpc, c, retval = EXIT_SUCCESS;

    auto_ptr<piper_proc> stdin_reader;
    const char *         stdin_out = NULL;
    int                  stdin_out_fd = -1;

    setlocale(LC_NUMERIC, "");

    lnav_data.ld_program_name = argv[0];

    rl_readline_name = "lnav";

    ensure_dotlnav();

    log_install_handlers();
    sql_install_logger();

    lnav_data.ld_debug_log_name = "/dev/null";
    while ((c = getopt(argc, argv, "hHarsCc:I:if:d:nqtw:VW")) != -1) {
        switch (c) {
        case 'h':
            usage();
            exit(retval);
            break;

        case 'H':
            lnav_data.ld_flags |= LNF_HELP;
            break;

        case 'C':
            lnav_data.ld_flags |= LNF_CHECK_CONFIG;
            break;

        case 'c':
            switch (optarg[0]) {
            case ':':
            case '/':
            case ';':
            case '|':
                break;
            default:
                fprintf(stderr, "error: command arguments should start with a "
                    "colon, semi-colon, or pipe-symbol to denote:\n");
                fprintf(stderr, "error: a built-in command, SQL query, "
                    "or a file path that contains commands to execute\n");
                usage();
                exit(EXIT_FAILURE);
                break;
            }
            lnav_data.ld_commands.push_back(optarg);
            break;

        case 'f':
            if (access(optarg, R_OK) != 0) {
                perror("invalid command file");
                exit(EXIT_FAILURE);
            }
            lnav_data.ld_commands.push_back("|" + string(optarg));
            break;

        case 'I':
            if (access(optarg, X_OK) != 0) {
                perror("invalid config path");
                exit(EXIT_FAILURE);
            }
            lnav_data.ld_config_paths.push_back(optarg);
            break;

            case 'i':
                lnav_data.ld_flags |= LNF_INSTALL;
                break;

        case 'd':
            lnav_data.ld_debug_log_name = optarg;
            break;

        case 'a':
            lnav_data.ld_flags |= LNF__ALL;
            break;

        case 'n':
            lnav_data.ld_flags |= LNF_HEADLESS;
            break;

        case 'q':
            lnav_data.ld_flags |= LNF_QUIET;
            break;

        case 'r':
            lnav_data.ld_flags |= LNF_ROTATED;
            break;

        case 's':
            lnav_data.ld_flags |= LNF_SYSLOG;
            break;

        case 't':
            lnav_data.ld_flags |= LNF_TIMESTAMP;
            break;

        case 'w':
            stdin_out = optarg;
            break;

        case 'W':
        {
            char b;
            read(STDIN_FILENO, &b, 1);
        }
            break;

        case 'V':
            printf("%s\n", PACKAGE_STRING);
            exit(0);
            break;

        default:
            retval = EXIT_FAILURE;
            break;
        }
    }

    argc -= optind;
    argv += optind;

    lnav_log_file = fopen(lnav_data.ld_debug_log_name, "a");

    if (lnav_data.ld_flags & LNF_INSTALL) {
        string installed_path = dotlnav_path("formats/installed/");

        if (argc == 0) {
            fprintf(stderr, "error: expecting file format paths\n");
            return EXIT_FAILURE;
        }

        for (lpc = 0; lpc < argc; lpc++) {
            vector<string> format_list = load_format_file(argv[lpc], loader_errors);

            if (!loader_errors.empty()) {
                print_errors(loader_errors);
                return EXIT_FAILURE;
            }
            if (format_list.empty()) {
                fprintf(stderr, "error: format file is empty: %s\n", argv[lpc]);
                return EXIT_FAILURE;
            }

            string dst_name = format_list[0] + ".json";
            string dst_path = installed_path + dst_name;
            auto_fd in_fd, out_fd;

            if ((in_fd = open(argv[lpc], O_RDONLY)) == -1) {
                perror("unable to open file to install");
            }
            else if ((out_fd = open(dst_path.c_str(),
                    O_WRONLY | O_CREAT, 0644)) == -1) {
                fprintf(stderr, "error: unable to open destination: %s -- %s\n",
                        dst_path.c_str(), strerror(errno));
            }
            else {
                char buffer[2048];
                ssize_t rc;

                while ((rc = read(in_fd, buffer, sizeof(buffer))) > 0) {
                    write(out_fd, buffer, rc);
                }

                fprintf(stderr, "info: installed: %s\n", dst_path.c_str());
            }
        }
        return EXIT_SUCCESS;
    }

    load_formats(lnav_data.ld_config_paths, loader_errors);
    if (!loader_errors.empty()) {
        print_errors(loader_errors);
        return EXIT_FAILURE;
    }

    if (lnav_data.ld_flags & LNF_CHECK_CONFIG) {
        return EXIT_SUCCESS;
    }

    /* If we statically linked against an ncurses library that had a non-
     * standard path to the terminfo database, we need to set this variable
     * so that it will try the default path.
     */
    setenv("TERMINFO_DIRS",
           "/usr/share/terminfo:/lib/terminfo:/usr/share/lib/terminfo",
           0);

    if (sqlite3_open(":memory:", lnav_data.ld_db.out()) != SQLITE_OK) {
        fprintf(stderr, "error: unable to create sqlite memory database\n");
        exit(EXIT_FAILURE);
    }

    {
        int register_collation_functions(sqlite3 * db);

        register_sqlite_funcs(lnav_data.ld_db.in(), sqlite_registration_funcs);
        register_collation_functions(lnav_data.ld_db.in());
    }

    register_environ_vtab(lnav_data.ld_db.in());

    lnav_data.ld_vtab_manager =
        new log_vtab_manager(lnav_data.ld_db,
                             lnav_data.ld_views[LNV_LOG],
                             lnav_data.ld_log_source,
                             sql_progress);

    {
        auto_mem<char, sqlite3_free> errmsg;

        if (sqlite3_exec(lnav_data.ld_db.in(),
                         init_sql,
                         NULL,
                         NULL,
                         errmsg.out()) != SQLITE_OK) {
            fprintf(stderr,
                    "error: unable to execute DB init -- %s\n",
                    errmsg.in());
        }
    }

    lnav_data.ld_vtab_manager->register_vtab(new log_vtab_impl("generic_log"));

    for (std::vector<log_format *>::iterator iter = log_format::get_root_formats().begin();
         iter != log_format::get_root_formats().end();
         ++iter) {
        log_vtab_impl *lvi = (*iter)->get_vtab_impl();

        if (lvi != NULL) {
            lnav_data.ld_vtab_manager->register_vtab(lvi);
        }
    }

    DEFAULT_FILES.insert(make_pair(LNF_SYSLOG, string("var/log/messages")));
    DEFAULT_FILES.insert(make_pair(LNF_SYSLOG, string("var/log/system.log")));
    DEFAULT_FILES.insert(make_pair(LNF_SYSLOG, string("var/log/syslog")));
    DEFAULT_FILES.insert(make_pair(LNF_SYSLOG, string("var/log/syslog.log")));

    init_lnav_commands(lnav_commands);

    lnav_data.ld_views[LNV_HELP].
    set_sub_source(new plain_text_source(help_txt));
    lnav_data.ld_views[LNV_HELP].set_word_wrap(true);
    lnav_data.ld_views[LNV_LOG].
    set_sub_source(&lnav_data.ld_log_source);
    lnav_data.ld_views[LNV_LOG].
    set_delegate(new action_delegate(lnav_data.ld_log_source));
    lnav_data.ld_views[LNV_TEXT].
    set_sub_source(&lnav_data.ld_text_source);
    lnav_data.ld_views[LNV_HISTOGRAM].
    set_sub_source(&lnav_data.ld_hist_source);
    lnav_data.ld_views[LNV_GRAPH].
    set_sub_source(&lnav_data.ld_graph_source);
    lnav_data.ld_views[LNV_DB].
    set_sub_source(&lnav_data.ld_db_source);
    lnav_data.ld_db_overlay.dos_labels = &lnav_data.ld_db_rows;
    lnav_data.ld_views[LNV_DB].
    set_overlay_source(&lnav_data.ld_db_overlay);
    lnav_data.ld_views[LNV_LOG].
    set_overlay_source(new field_overlay_source(lnav_data.ld_log_source));
    lnav_data.ld_db_overlay.dos_hist_source = &lnav_data.ld_db_source;

    lnav_data.ld_match_view.set_left(0);

    for (int lpc = 0; lpc < LNV__MAX; lpc++) {
        lnav_data.ld_views[lpc].set_gutter_source(new log_gutter_source());
    }

    {
        setup_highlights(lnav_data.ld_views[LNV_LOG].get_highlights());
        setup_highlights(lnav_data.ld_views[LNV_TEXT].get_highlights());
        setup_highlights(lnav_data.ld_views[LNV_SCHEMA].get_highlights());
        setup_highlights(lnav_data.ld_views[LNV_PRETTY].get_highlights());
    }

    {
        hist_source &hs = lnav_data.ld_hist_source;

        lnav_data.ld_hist_zoom = 2;
        hs.set_role_for_type(bucket_type_t(logline::LEVEL_FATAL),
           view_colors::VCR_ERROR);
        hs.set_role_for_type(bucket_type_t(logline::LEVEL_CRITICAL),
           view_colors::VCR_ERROR);
        hs.set_role_for_type(bucket_type_t(logline::LEVEL_ERROR),
           view_colors::VCR_ERROR);
        hs.set_role_for_type(bucket_type_t(logline::LEVEL_WARNING),
           view_colors::VCR_WARNING);
        hs.set_label_source(new time_label_source());
    }

    {
        hist_source &hs = lnav_data.ld_graph_source;

        hs.set_bucket_size(1);
        hs.set_group_size(100);
    }

    {
        hist_source &hs = lnav_data.ld_db_source;

        hs.set_bucket_size(1);
        hs.set_group_size(10);
        hs.set_label_source(&lnav_data.ld_db_rows);
    }

    for (int lpc = 0; lpc < LNV__MAX; lpc++) {
        lnav_data.ld_views[lpc].set_title(view_titles[lpc]);
    }

    lnav_data.ld_looping        = true;
    lnav_data.ld_mode           = LNM_PAGING;

    if (isatty(STDIN_FILENO) && argc == 0 &&
        !(lnav_data.ld_flags & LNF__ALL)) {
        lnav_data.ld_flags |= LNF_SYSLOG;
    }

    if (lnav_data.ld_flags != 0) {
        char start_dir[FILENAME_MAX];

        if (getcwd(start_dir, sizeof(start_dir)) == NULL) {
            perror("getcwd");
        }
        else {
            do {
                for (lpc = 0; lpc < LNB__MAX; lpc++) {
                    if (!append_default_files((lnav_flags_t)(1L << lpc))) {
                        retval = EXIT_FAILURE;
                    }
                }
            } while (lnav_data.ld_file_names.empty() &&
                     change_to_parent_dir());

            if (chdir(start_dir) == -1) {
                perror("chdir(start_dir)");
            }
        }
    }

    for (lpc = 0; lpc < argc; lpc++) {
        auto_mem<char> abspath;
        struct stat    st;

        if (is_glob(argv[lpc])) {
            lnav_data.ld_file_names.insert(make_pair(argv[lpc], -1));
        }
        else if (stat(argv[lpc], &st) == -1) {
            fprintf(stderr,
                    "Cannot stat file: %s -- %s\n",
                    argv[lpc],
                    strerror(errno));
            retval = EXIT_FAILURE;
        }
        else if ((abspath = realpath(argv[lpc], NULL)) == NULL) {
            perror("Cannot find file");
            retval = EXIT_FAILURE;
        }
        else if (S_ISDIR(st.st_mode)) {
            string dir_wild(abspath.in());

            if (dir_wild[dir_wild.size() - 1] == '/') {
                dir_wild.resize(dir_wild.size() - 1);
            }
            lnav_data.ld_file_names.insert(make_pair(dir_wild + "/*", -1));
        }
        else {
            lnav_data.ld_file_names.insert(make_pair(abspath.in(), -1));
        }
    }

    if (!(lnav_data.ld_flags & LNF_HEADLESS) && !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "error: stdout is not a tty.\n");
        retval = EXIT_FAILURE;
    }

    if (!isatty(STDIN_FILENO)) {
        stdin_reader =
            auto_ptr<piper_proc>(new piper_proc(STDIN_FILENO,
                                                lnav_data.ld_flags &
                                                LNF_TIMESTAMP, stdin_out));
        stdin_out_fd = stdin_reader->get_fd();
        lnav_data.ld_file_names.insert(make_pair("stdin", stdin_out_fd));
        if (dup2(STDOUT_FILENO, STDIN_FILENO) == -1) {
            perror("cannot dup stdout to stdin");
        }
    }

    if (lnav_data.ld_file_names.empty() && !(lnav_data.ld_flags & LNF_HELP)) {
        fprintf(stderr, "error: no log files given/found.\n");
        retval = EXIT_FAILURE;
    }

    if (retval != EXIT_SUCCESS) {
        usage();
    }
    else {
        try {
            rescan_files(true);

            log_info("startup: %s", PACKAGE_STRING);
            log_host_info();
            log_info("Libraries:");
#ifdef HAVE_BZLIB_H
            log_info("  bzip=%s", BZ2_bzlibVersion());
#endif
            log_info("  ncurses=%s", NCURSES_VERSION);
            log_info("  pcre=%s", pcre_version());
            log_info("  readline=%s", rl_library_version);
            log_info("  sqlite=%s", sqlite3_version);
            log_info("  zlib=%s", zlibVersion());
            log_info("lnav_data:");
            log_info("  flags=%x", lnav_data.ld_flags);
            log_info("  commands:");
            for (std::list<string>::iterator cmd_iter =
                 lnav_data.ld_commands.begin();
                 cmd_iter != lnav_data.ld_commands.end();
                 ++cmd_iter) {
                log_info("    %s", cmd_iter->c_str());
            }
            log_info("  files:");
            for (std::set<pair<string, int> >::iterator file_iter =
                 lnav_data.ld_file_names.begin();
                 file_iter != lnav_data.ld_file_names.end();
                 ++file_iter) {
                log_info("    %s", file_iter->first.c_str());
            }

            if (lnav_data.ld_flags & LNF_HEADLESS) {
                std::vector<pair<string, string> > msgs;
                std::vector<pair<string, string> >::iterator msg_iter;
                textview_curses *tc;
                attr_line_t al;
                const std::string &line = al.get_string();
                bool found_error = false;

                alerter::singleton().enabled(false);

                lnav_data.ld_view_stack.push(&lnav_data.ld_views[LNV_LOG]);
                rebuild_indexes(true);

                lnav_data.ld_views[LNV_LOG].set_top(vis_line_t(0));

                execute_init_commands(msgs);
                for (;;) {
                    gather_pipers();
                    if (lnav_data.ld_pipers.empty()) {
                        break;
                    }
                    else {
                        usleep(10000);
                        rebuild_indexes(false);
                    }
                }
                rebuild_indexes(false);

                for (msg_iter = msgs.begin();
                     msg_iter != msgs.end();
                     ++msg_iter) {
                    if (strncmp("error:", msg_iter->first.c_str(), 6) != 0) {
                        continue;
                    }

                    fprintf(stderr, "%s\n", msg_iter->first.c_str());
                    found_error = true;
                }

                if (!found_error &&
                    !(lnav_data.ld_flags & LNF_QUIET) &&
                    !lnav_data.ld_view_stack.empty() &&
                    !lnav_data.ld_stdout_used) {
                    bool suppress_empty_lines = false;
                    list_overlay_source *los;
                    vis_line_t y;

                    tc = lnav_data.ld_view_stack.top();
                    if (tc == &lnav_data.ld_views[LNV_DB]) {
                        suppress_empty_lines = true;
                    }

                    los = tc->get_overlay_source();

                    for (vis_line_t vl = tc->get_top();
                         vl < tc->get_inner_height();
                         ++vl, ++y) {
                        while (los != NULL &&
                               los->list_value_for_overlay(*tc, y, al)) {
                            printf("%s\n", line.c_str());
                            ++y;
                        }

                        tc->listview_value_for_row(*tc, vl, al);
                        if (suppress_empty_lines && line.empty()) {
                            continue;
                        }

                        printf("%s\n", line.c_str());
                    }
                }
            }
            else {
                init_session();

                log_info("  session_id=%s", lnav_data.ld_session_id.c_str());

                scan_sessions();

                guard_termios gt(STDIN_FILENO);

                lnav_log_orig_termios = gt.get_termios();

                looper();

                save_session();
            }
        }
        catch (line_buffer::error & e) {
            fprintf(stderr, "error: %s\n", strerror(e.e_err));
        }
        catch (logfile::error & e) {
            if (e.e_err != EINTR) {
                fprintf(stderr,
                        "error: %s -- '%s'\n",
                        strerror(e.e_err),
                        e.e_filename.c_str());
            }
        }

        // When reading from stdin, dump out the last couple hundred lines so
        // the user can have the text in their terminal history.
        if (stdin_out_fd != -1) {
            list<logfile *>::iterator file_iter;
            struct stat st;

            fstat(stdin_out_fd, &st);
            file_iter = find_if(lnav_data.ld_files.begin(),
                                lnav_data.ld_files.end(),
                                same_file(st));
            if (file_iter != lnav_data.ld_files.end()) {
                logfile::iterator line_iter;
                logfile *lf = *file_iter;
                string str;

                for (line_iter = lf->begin();
                     line_iter != lf->end();
                     ++line_iter) {
                    lf->read_line(line_iter, str);

                    write(STDOUT_FILENO, str.c_str(), str.size());
                    write(STDOUT_FILENO, "\n", 1);
                }
            }
        }
    }

    return retval;
}
