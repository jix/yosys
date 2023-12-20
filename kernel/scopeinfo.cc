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

#include "kernel/scopeinfo.h"

USING_YOSYS_NAMESPACE

using Scope = ScopeinfoIndex::Scope;
using ScopeInfo = ScopeinfoIndex::Scopeinfo;
using ScopeEntry = ScopeinfoIndex::ScopeEntry;
using ScopeData = ScopeinfoIndex::ScopeData;
using ScopeEntryData = ScopeinfoIndex::ScopeEntryData;
using Options = ScopeinfoIndex::Options;


ScopeData::ScopeData(ScopeEntry entry, Scopeinfo scopeinfo) :
	entry(entry), repr(ScopeData::Repr::SCOPEINFO)
{
	data.scopeinfo = scopeinfo.cell;
	std::string scopeinfo_type = scopeinfo.cell->getParam(ID::TYPE).decode_string();
	if (scopeinfo_type == "module")
		type = ScopeData::Type::MODULE;
	else if (scopeinfo_type == "struct")
		type = ScopeData::Type::STRUCT;
	else
		log_assert(false);
}

ScopeData::ScopeData(ScopeEntry entry, RTLIL::Module *module) :
	entry(entry), type(ScopeData::Type::MODULE), repr(ScopeData::Repr::MODULE)
{
	data.module = module;
}

ScopeData::ScopeData(ScopeEntry entry) :
	entry(entry), type(ScopeData::Type::MODULE), repr(ScopeData::Repr::IMPLICIT)
{
}

template<typename T>
static std::vector<RTLIL::IdString> parse_hdlname(T *object)
{
	std::vector<RTLIL::IdString> hdlname;
	std::string segment;
	hdlname.clear();
	if (object->has_attribute(ID::hdlname)) {
		std::string hdlname_str = object->get_string_attribute(ID::hdlname);
		segment.clear();
		for (char c : hdlname_str) {
			if (c == ' ') {
				hdlname.emplace_back(RTLIL::escape_id(segment));
				segment.clear();
			} else {
				segment.push_back(c);
			}
		}
		hdlname.emplace_back(RTLIL::escape_id(segment));
	} else {
		hdlname.emplace_back(object->name);
	}
	return hdlname;
}

RTLIL::IdString ScopeData::name() const
{
	switch (repr) {
		case Repr::IMPLICIT: return IdString();
		case Repr::MODULE: return data.module->name;
		case Repr::SCOPEINFO: {
			switch (type) {
				case Type::MODULE:
					return RTLIL::escape_id(data.scopeinfo->get_string_attribute(ID(module)));
				case Type::STRUCT:
					return RTLIL::escape_id(data.scopeinfo->get_string_attribute(ID(struct)));
				default:
					log_assert(false);
			}
		}
		default:
			log_assert(false);
	}
}

ScopeEntryData::ScopeEntryData(Scope scope, RTLIL::IdString name, Scopeinfo scopeinfo) :
	scope(scope), name(name), repr(ScopeEntryData::Repr::SCOPEINFO)
{
	data.scopeinfo = scopeinfo.cell;
	std::string scopeinfo_type = scopeinfo.cell->getParam(ID::TYPE).decode_string();
	 if (scopeinfo_type == "module")
		type = ScopeEntryData::Type::CELL;
	else if (scopeinfo_type == "struct")
		type = ScopeEntryData::Type::WIRE;
	else
		log_assert(false);
}

ScopeEntryData::ScopeEntryData(Scope scope, RTLIL::IdString name, RTLIL::Cell *cell) :
	scope(scope), name(name),
	type(ScopeEntryData::Type::CELL), repr(ScopeEntryData::Repr::CELL)
{
	data.cell = cell;
}

ScopeEntryData::ScopeEntryData(Scope scope, RTLIL::IdString name, RTLIL::Wire *wire) :
	scope(scope), name(name),
	type(ScopeEntryData::Type::WIRE), repr(ScopeEntryData::Repr::WIRE)
{
	data.wire = wire;
}

ScopeEntryData::ScopeEntryData(Scope scope, RTLIL::IdString name) :
	scope(scope), name(name),
	type(ScopeEntryData::Type::CELL), repr(ScopeEntryData::Repr::IMPLICIT)
{
}


RTLIL::IdString ScopeEntryData::scope_name() const
{
	switch (repr) {
		case Repr::IMPLICIT: return IdString();
		case Repr::CELL: return data.cell->type;
		case Repr::WIRE: return IdString();
		case Repr::SCOPEINFO: {
			switch (type) {
				case Type::CELL:
					return RTLIL::escape_id(data.scopeinfo->get_string_attribute(ID(module)));
				case Type::WIRE:
					return RTLIL::escape_id(data.scopeinfo->get_string_attribute(ID(struct)));
				default:
					log_assert(false);
			}
		}
		default:
			log_assert(false);
	}
}

ScopeinfoIndex::ScopeinfoIndex(ScopeinfoIndex::Options options, RTLIL::Module *module) :
	options(options)
{
	index_module(ScopeEntry(), module);
}

ScopeinfoIndex::ScopeinfoIndex(ScopeinfoIndex::Options options, RTLIL::Design *design) :
	options(options)
{
	log_assert(design->top_module());
	index_module(ScopeEntry(), design->top_module());
}

ScopeData const &ScopeinfoIndex::data(Scope scope) const
{
	log_assert(scope.valid() && scope.index <= GetSize(scopes));
	return scopes[scope.index];
}

ScopeEntryData const &ScopeinfoIndex::data(ScopeEntry entry) const
{
	log_assert(entry.valid() && entry.index <= GetSize(scope_entries));
	return scope_entries[entry.index];
}

void ScopeinfoIndex::index_module(ScopeEntry entry, RTLIL::Module *module)
{
	Scope scope(GetSize(scopes));
	scopes.emplace_back(entry, module);

	if (entry.valid())
		scope_entries[entry.index].inner = scope;


	std::vector<std::tuple<int, IdString, Scopeinfo>> scopeinfos;

	for (auto cell : module->cells()) {
		if (cell->type == ID($scopeinfo)) {
			int depth = 1;
			if (cell->has_attribute(ID::hdlname))
				for (char c : cell->get_string_attribute(ID::hdlname))
					depth += (c == ' ');
			scopeinfos.push_back(std::make_tuple(depth, cell->name, Scopeinfo(cell)));
		}
	}

	std::sort(scopeinfos.begin(), scopeinfos.end());

	std::vector<RTLIL::IdString> hdlname;
	std::string segment;

	for (auto tuple : scopeinfos)
	{
		Scopeinfo scopeinfo = std::get<2>(tuple);

		hdlname = parse_hdlname(scopeinfo.cell);
		RTLIL::IdString leaf_name = hdlname.back();
		hdlname.pop_back();

		Scope subscope = scope;
		for (auto name : hdlname)
			subscope = enter_or_create_scope(subscope, name);

		index_scopeinfo(subscope, leaf_name, scopeinfo);
	}

	if (options.index_cells)
	{
		for (auto cell : module->cells())
		{
			if (cell->type == ID($scopeinfo))
				continue;
			if (!cell->name.isPublic())
				continue;

			hdlname = parse_hdlname(cell);
			RTLIL::IdString leaf_name = hdlname.back();
			hdlname.pop_back();

			Scope subscope = scope;
			for (auto name : hdlname)
				subscope = enter_or_create_scope(subscope, name);

			ScopeEntry instance_entry(GetSize(scope_entries));
			scope_entries.emplace_back(subscope, leaf_name, cell);
			scopes[subscope.index].entries.emplace(leaf_name, instance_entry);

			if (options.lookup_cells)
			{
				auto &cell_index_ref = cell_entry_index[cell];
				if (cell_index_ref.valid())
					scope_entries[instance_entry.index].next_location = cell_index_ref;
				cell_index_ref = instance_entry;
			}

			if (!options.index_instances)
				continue;
			RTLIL::Module *instance_module = module->design->module(cell->type);
			if (!instance_module)
				continue;
			index_module(instance_entry, instance_module);
		}
	}

	if (options.index_wires)
	{
		for (auto wire : module->wires())
		{
			if (!wire->name.isPublic())
				continue;

			hdlname = parse_hdlname(wire);
			RTLIL::IdString leaf_name = hdlname.back();
			hdlname.pop_back();

			Scope subscope = scope;
			for (auto name : hdlname)
				subscope = enter_or_create_scope(subscope, name);

			ScopeEntry wire_entry(GetSize(scope_entries));
			scope_entries.emplace_back(subscope, leaf_name, wire);
			scopes[subscope.index].entries.emplace(leaf_name, wire_entry);


			if (options.lookup_wires)
			{
				auto &cell_index_ref = wire_entry_index[wire];
				if (cell_index_ref.valid())
					scope_entries[wire_entry.index].next_location = cell_index_ref;
				cell_index_ref = wire_entry;
			}
		}
	}
}

void ScopeinfoIndex::index_scopeinfo(Scope scope, RTLIL::IdString name, Scopeinfo scopeinfo)
{
	log_assert(scope.valid() && scope.index <= GetSize(scopes));
	log_assert(!scopes[scope.index].entries.count(name));

	ScopeEntry entry(GetSize(scope_entries));
	Scope inner(GetSize(scopes));
	scope_entries.emplace_back(scope, name, scopeinfo);
	scope_entries.back().inner = inner;
	scopes.emplace_back(entry, scopeinfo);
	scopes[scope.index].entries.emplace(name, entry);

	if (options.lookup_cells)
	{
		auto &cell_index_ref = cell_entry_index[scopeinfo.cell];
		if (cell_index_ref.valid())
			scope_entries[entry.index].next_location = cell_index_ref;
		cell_index_ref = entry;
	}
}

Scope ScopeinfoIndex::enter_or_create_scope(Scope scope, RTLIL::IdString name)
{
	log_assert(scope.valid() && scope.index <= GetSize(scopes));
	if (scopes[scope.index].entries.count(name)) {
		ScopeEntry entry = scopes[scope.index].entries[name];
		ScopeEntryData entry_data = scope_entries[entry.index];
		log_assert(entry_data.inner.valid());
		return entry_data.inner;
	} else {
		if (!options.implicit_scopes)
			log_error("Missing $scopeinfo cell with implicit scopes disabled."); // XXX better error
		ScopeEntry entry(GetSize(scope_entries));
		Scope inner(GetSize(scopes));
		scope_entries.emplace_back(scope, name);
		scope_entries.back().inner = inner;
		scopes.emplace_back(entry);

		scopes[scope.index].entries.emplace(name, entry);
		return inner;
	}
}

ScopeEntry ScopeinfoIndex::lookup_entry(RTLIL::Wire *wire) const
{
	auto found = wire_entry_index.find(wire);
	if (found == wire_entry_index.end())
		return ScopeEntry();
	return found->second;
}


ScopeEntry ScopeinfoIndex::lookup_entry(RTLIL::Cell *wire) const
{
	auto found = cell_entry_index.find(wire);
	if (found == cell_entry_index.end())
		return ScopeEntry();
	return found->second;
}

std::string ScopeinfoIndex::hdlname(ScopeEntry entry)
{
	std::string result;
	std::vector<IdString> names;
	while (entry.valid())
	{
		const auto &entry_data = data(entry);
		names.push_back(entry_data.entry_name());
		entry = data(entry_data.scope).entry;
	}

	for (auto i = names.rbegin(), i_end = names.rend(); i != i_end; ++i)
	{
		if (!result.empty())
			result.push_back(' ');
		result += unescape_id(*i);
	}

	return result;
}
