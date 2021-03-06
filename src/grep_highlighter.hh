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
 */
#ifndef __grep_highlighter_hh
#define __grep_highlighter_hh

#include <string>
#include <memory>

#include "grep_proc.hh"
#include "textview_curses.hh"

class grep_highlighter {
public:
    grep_highlighter(std::auto_ptr<grep_proc> gp,
                     std::string hl_name,
                     textview_curses::highlight_map_t &hl_map)
        : gh_grep_proc(gp),
          gh_hl_name(hl_name),
          gh_hl_map(hl_map) { };

    ~grep_highlighter()
    {
        this->gh_hl_map.erase(this->gh_hl_map.find(this->gh_hl_name));
    };

    grep_proc *get_grep_proc() { return this->gh_grep_proc.get(); };

private:
    std::auto_ptr<grep_proc> gh_grep_proc;
    std::string gh_hl_name;
    textview_curses::highlight_map_t &gh_hl_map;
};
#endif
