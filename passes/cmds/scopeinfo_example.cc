/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2023  Jannis Harder <jix@yosyshq.com> <me@jix.one>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */


#include "kernel/yosys.h"
#include "kernel/scopeinfo.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN


static void dump_scope(int depth, const ScopeinfoIndex &info, ScopeinfoIndex::Scope scope);

static void dump_entry(int depth, const ScopeinfoIndex &info, ScopeinfoIndex::ScopeEntry entry)
{
    const char *type;
    const char *scope_type;
    switch (info.data(entry).type) {
        case ScopeinfoIndex::ScopeEntryData::Type::CELL: type = "cell"; scope_type = "module"; break;
        case ScopeinfoIndex::ScopeEntryData::Type::WIRE: type = "wire"; scope_type = "struct"; break;
        default: log_assert(false);
    }
    log("%*s- %s %s\n", depth, "", type, RTLIL::unescape_id(info.data(entry).name).c_str());
    if (info.data(entry).inner.valid()) {
        dump_scope(depth + 2, info, info.data(entry).inner);
    } else {
        IdString scope_name = info.data(entry).scope_name();
        if (!scope_name.empty())
            log("%*s= %s %s\n", depth + 2, "", scope_type, RTLIL::unescape_id(scope_name).c_str());
    }
}

static void dump_scope(int depth, const ScopeinfoIndex &info, ScopeinfoIndex::Scope scope) {
    const char *type;
    switch (info.data(scope).type) {
        case ScopeinfoIndex::ScopeData::Type::MODULE: type = "module"; break;
        case ScopeinfoIndex::ScopeData::Type::STRUCT: type = "struct"; break;
        default: log_assert(false);
    }
    log("%*s+ %s %s\n", depth, "", type, RTLIL::unescape_id(info.data(scope).name()).c_str());
    for (auto entry : info.data(scope).entries) {
        dump_entry(depth + 2, info, entry.second);
    }
};

struct ScopeinfoPass : public Pass {
	ScopeinfoPass() : Pass("scopeinfo_example", "dump scopeinfo") {}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    scopeinfo_example [options] [selection]\n");
		log("\n");
	}


	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing SCOPEINFO_EXAMPLE pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			break;
		}

		extra_args(args, argidx, design);

        ScopeinfoIndex info({}, design);

        dump_scope(0, info, info.top_scope());

		for (auto module : design->selected_modules()) {
            log("looking up cells in module %s\n", log_id(module->name));

            for (auto cell : module->selected_cells())
            {
                int counter = 0;
                for (auto entry : info.lookup_entries(cell))
                {
                    log("  %d found %s (%s) -> %s\n", counter++, log_id(cell), log_id(cell->type), info.hdlname(entry).c_str());
                }
            }

            log("looking up wires in module %s\n", log_id(module->name));

            for (auto wire : module->selected_wires())
            {
                int counter = 0;
                for (auto entry : info.lookup_entries(wire))
                {
                    log("  %d found %s -> %s\n", counter++, log_id(wire), info.hdlname(entry).c_str());
                }
            }

        }

        log("done\n");
	}
} XpropPass;

PRIVATE_NAMESPACE_END
