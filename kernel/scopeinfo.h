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

#ifndef SCOPEINFO_H
#define SCOPEINFO_H

#include "kernel/yosys.h"
#include "kernel/celltypes.h"

YOSYS_NAMESPACE_BEGIN

struct ScopeinfoIndex {
	struct Options {
		bool implicit_scopes = true;
		bool index_cells = true;
		bool index_wires = true;
		bool index_instances = true;
		bool lookup_cells = true;
		bool lookup_wires = true;
	};

	ScopeinfoIndex(Options options, RTLIL::Design *design);
	ScopeinfoIndex(Options options, RTLIL::Module *module);

	struct Scope {
		int index;

		explicit Scope(int index = -1) : index(index) {}
		bool valid() const { return index >= 0; }

		bool operator==(const Scope &other) const { return index == other.index; }
		unsigned int hash() const { return hash_ops<int>::hash(index); }
	};

	struct ScopeEntry {
		int index;

		explicit ScopeEntry(int index = -1) : index(index) {}
		bool valid() const { return index >= 0; }

		bool operator==(const Scope &other) const { return index == other.index; }
		unsigned int hash() const { return hash_ops<int>::hash(index); }
	};

	struct Scopeinfo {
		Cell *cell;
		explicit Scopeinfo(Cell *cell = nullptr) : cell(cell)
		{
			if (cell)
				log_assert(cell->type == ID($scopeinfo));
		}

		operator bool() const { return cell; }
	};

	struct ScopeData {
		enum class Type {
			MODULE, // Module scope with wires and cells as entries
			STRUCT, // Struct scope with fields as entries
		};

		enum class Repr {
			SCOPEINFO, // Module or struct scope represented by a $scopeinfo cell
			MODULE, // Module scope represented by an RTLIL::Module *
			IMPLICIT, // Module scope implicitly created for hdlnames attributes without matching $scopeinfo
		};

		ScopeEntry entry;
		Type type;
		Repr repr;
		union {
			RTLIL::Cell *scopeinfo;
			RTLIL::Module *module;
		} data;
		dict<RTLIL::IdString, ScopeEntry> entries;

		ScopeData(ScopeEntry entity, Scopeinfo scopeinfo);
		ScopeData(ScopeEntry entity, RTLIL::Module *module);
		ScopeData(ScopeEntry entity);

		RTLIL::IdString name() const;
	};

	struct ScopeEntryData {
		enum class Type {
			CELL, // Cell within a module
			WIRE, // Wire within a module or field within a struct
		};

		enum class Repr {
			SCOPEINFO, // Cell or struct-typed struct field represented by a $scopeinfo
			IMPLICIT, // Cell implicitly created for hdlnames attributes without matching $scopeinfo
			CELL, // Cell represented by an RTLIL::Cell *
			WIRE, // Wire or non-struct-typed struct field represented by an RTLIL::Wire *
		};

		Scope scope;
		IdString name;
		Type type;
		Repr repr;
		union {
			RTLIL::Cell *scopeinfo;
			RTLIL::Cell *cell;
			RTLIL::Wire *wire;
		} data;
		Scope inner;
		ScopeEntry next_location; // Linked list of all entries with the same data pointer

		ScopeEntryData(Scope scope, RTLIL::IdString name, Scopeinfo scopeinfo);
		ScopeEntryData(Scope scope, RTLIL::IdString name, RTLIL::Cell *cell);
		ScopeEntryData(Scope scope, RTLIL::IdString name, RTLIL::Wire *wire);
		ScopeEntryData(Scope scope, RTLIL::IdString name);

		RTLIL::IdString scope_name() const;
		RTLIL::IdString entry_name() const { return name; };
	};

	Scope top_scope() const { log_assert(!scopes.empty()); return Scope(0); }
	ScopeData const &data(Scope scope) const;
	ScopeEntryData const &data(ScopeEntry entry) const;

	ScopeEntry lookup_entry(RTLIL::Wire *wire) const;
	ScopeEntry lookup_entry(RTLIL::Cell *cell) const;

	template<typename T>
	std::vector<ScopeEntry> lookup_entries(T key) const;

	ScopeEntry next_location(ScopeEntry entry) const { return data(entry).next_location; }

	std::string hdlname(ScopeEntry entry);

private:
	Options options;

	std::vector<ScopeData> scopes;
	std::vector<ScopeEntryData> scope_entries;

	dict<RTLIL::Wire *, ScopeEntry> wire_entry_index;
	dict<RTLIL::Cell *, ScopeEntry> cell_entry_index;

	void index_module(ScopeEntry entry, RTLIL::Module *module);
	void index_scopeinfo(Scope scope, RTLIL::IdString name, Scopeinfo scopeinfo);

	Scope enter_or_create_scope(Scope scope, RTLIL::IdString name);
};


template<typename T>
std::vector<ScopeinfoIndex::ScopeEntry> ScopeinfoIndex::lookup_entries(T key) const
{
	std::vector<ScopeEntry> result;

	auto entry = lookup_entry(key);
	while (entry.valid()) {
		result.push_back(entry);
		entry = next_location(entry);
	}

	return result;
}

YOSYS_NAMESPACE_END

#endif
