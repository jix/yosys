/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
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
#include "kernel/sigtools.h"
#include "kernel/modtools.h"

#include <string.h>
#include <algorithm>
#include <optional>

YOSYS_NAMESPACE_BEGIN


void RTLIL::Design::bufNormalize(bool enable)
{
	if (!enable)
	{
		if (!flagBufferedNormalized)
			return;

		for (auto module : modules()) {
			module->buf_norm_cell_queue.clear();
			module->buf_norm_wire_queue.clear();
			module->buf_norm_cell_port_queue.clear();
			for (auto wire : module->wires()) {
				wire->driverCell_ = nullptr;
				wire->driverPort_ = IdString();
			}
		}

		flagBufferedNormalized = false;
		return;
	}

	if (!flagBufferedNormalized)
	{
		for (auto module : modules())
		{
			// When entering buf normalized mode, we need the first module-level bufNormalize
			// call to know about all drivers, about all module ports (whether represented by
			// a cell or not) and about all used but undriven wires (whether represented by a
			// cell or not). We ensure this by enqueing all cell output ports and all wires.

			for (auto cell : module->cells())
			for (auto &conn : cell->connections()) {
				if (GetSize(conn.second) == 0 || cell->port_dir(conn.first) != RTLIL::PD_OUTPUT)
					continue;
				module->buf_norm_cell_queue.insert(cell);
				module->buf_norm_cell_port_queue.emplace(cell, conn.first);
			}
			for (auto wire : module->wires())
				module->buf_norm_wire_queue.insert(wire);

		}

		flagBufferedNormalized = true;
	}

	for (auto module : modules())
		module->bufNormalize();
}

struct bit_drive_data_t {
	int drivers = 0;
	int inout = 0;
	int users = 0;
};

typedef ModWalker::PortBit PortBit;


struct SigBitGraph {
	dict<SigBit, SigBit> first_connection;
	dict<SigBit, SigBit> second_connection;
	dict<SigBit, idict<SigBit>> more_connections;

	void add_half_connection(SigBit const &a, SigBit const &b) {
		if (first_connection.emplace(a, b).second)
			return;
		if (second_connection.emplace(a, b).second)
			return;
		more_connections[a](b);
	}

	bool has_connection(SigBit const &a, SigBit const &b) {
		auto found = first_connection.find(a);
		if (found == first_connection.end())
			return false;
		if (found->second == b)
			return true;
		found = second_connection.find(a);
		if (found == second_connection.end())
			return false;
		if (found->second == b)
			return true;
		auto found2 = more_connections.find(a);
		if (found2 == more_connections.end())
			return false;
		return found2->second.count(b);
	}

	void add_connection(SigBit const &a, SigBit const &b) {
		add_half_connection(a, b);
		add_half_connection(b, a);
	}


	void add_connection(SigSpec const &a, SigSpec const &b) {
		log_assert(GetSize(a) == GetSize(b));
		for (int i = 0; i != GetSize(a); ++i)
			add_connection(a[i], b[i]);
	}

	int count_connections(SigBit const &a) {
		if (!first_connection.count(a))
			return 0;
		if (!second_connection.count(a))
			return 1;
		auto found = more_connections.find(a);
		if (found == more_connections.end())
			return 2;
		return GetSize(found->second) + 2;
	}

	SigBit const &nth_connection(SigBit const &a, int n) {
		if (n == 0)
			return first_connection.at(a);
		if (n == 1)
			return second_connection.at(a);
		return more_connections.at(a)[n - 2];
	}
};


static bool keep_wire(Wire *wire)
{
	if (wire->name.isPublic() || wire->port_input || wire->port_output)
		return true;
	int count = wire->attributes.size();
	if (!count)
		return false;
	count -= wire->attributes.count(ID::src);
	if (!count)
		return false;
	count -= wire->attributes.count(ID::hdlname);
	if (!count)
		return false;
	count -= wire->attributes.count(ID(scopename));
	if (!count)
		return false;
	count -= wire->attributes.count(ID::unused_bits);
	return count;
}

template<class T>
struct bfs_queue {
	idict<T> &entries;
	int pos = 0;

	bfs_queue(idict<T> &entries) : entries(entries) {};

	bool finished() const { return pos >= GetSize(entries); }
	T const &current() const { log_assert(!finished()); return entries[pos]; }
	T const &advance() { log_assert(!finished()); return entries[pos++]; }

	bool enqueue(T const &item) { int size = GetSize(entries); return size == entries(item); }
};

void RTLIL::Module::bufNormalize()
{
	if (!design->flagBufferedNormalized)
		return;

	int iterations = 0;

	while (!buf_norm_cell_queue.empty() || !buf_norm_wire_queue.empty() || !connections_.empty())
	{
		if (iterations++)
			log_warning("XXX didn't converge %d\n", iterations);

		log("XXX %d cell_queue\n", GetSize(buf_norm_cell_queue));
		for (auto cell : buf_norm_cell_queue)
			log("XXX cell %s\n", log_id(cell));
		log("XXX %d cell_port_queue\n", GetSize(buf_norm_cell_port_queue));
		log("XXX %d wire_queue\n", GetSize(buf_norm_wire_queue));
		log("XXX %d connections\n", GetSize(connections_));

		// XXX is this always guaranteed to converge in a single iteration?
		if (iterations >= 2) {
			log_warning("XXX giving up!\n");
			return;
		}

		for (auto const &[cell, port_name] : buf_norm_cell_port_queue) {
			if (cell->type != ID($input_port))
				continue;
			SigSpec const &sig = cell->getPort(ID::Y);
			if (!sig.is_wire()) {
				remove(cell);
				continue;
			}
			Wire *w = sig.as_wire();
			w->driverCell_ = cell;
			w->driverPort_ = ID::Y;
		}

		// Ensure that every enqueued input port is represented by a cell
		for (auto wire : buf_norm_wire_queue) {
			if (wire->port_input && !wire->port_output) {
				if (wire->driverCell_ != nullptr && wire->driverCell_->type != ID($input_port)) {
					wire->driverCell_ = nullptr;
					wire->driverPort_.clear();
				}
				if (wire->driverCell_ == nullptr) {
					Cell *input_port_cell = addCell(NEW_ID, ID($input_port));
					input_port_cell->setParam(ID::WIDTH, GetSize(wire));
					input_port_cell->setPort(ID::Y, wire); // this hits the fast path that doesn't mutate the queues

				}
			}
		}

		idict<Wire *> wire_queue_entries;

		int wire_queue_pos = 0;
		pool<Wire *> direct_driven_wires;

		pool<SigBit> zbits = {State::Sz};

		auto enqueue_cell_port = [&](Cell *cell, IdString port) {
			log("processing cell port %s.%s\n", log_id(cell), log_id(port));
			if (cell->type.empty())
				return;
			SigSpec const &sig = cell->getPort(port);
			if (cell->type == ID($input_port)) {
				log_assert(port == ID::Y);
				if (!sig.is_wire()) {
					buf_norm_cell_queue.insert(cell);
					remove(cell);
					return;
				}
				Wire *w = sig.as_wire();
				if (!w->port_input || w->port_output) {
					buf_norm_cell_queue.insert(cell);
					remove(cell);
					return;
				}
				w->driverCell_ = cell;
				w->driverPort_ = ID::Y;
			} else if (cell->type == ID($buf) && cell->attributes.empty() && !cell->name.isPublic()) {
				log_assert(port == ID::Y);
				SigSpec sig_a = cell->getPort(ID::A);
				SigSpec sig_y = sig;

				for (auto const &s : {sig_a, sig})
					for (auto const &chunk : s.chunks())
						if (chunk.wire)
							wire_queue_entries(chunk.wire);

				if (sig_a.has_const(State::Sz)) {
					SigSpec new_a;
					SigSpec new_y;
					for (int i = 0; i < GetSize(sig_a); ++i) {
						SigBit b = sig_a[i];
						if (b == State::Sz)
							continue;
						new_a.append(b);
						new_y.append(sig_y[i]);
					}
					sig_a = std::move(new_a);
					sig_y = std::move(new_y);
				}

				if (!sig_y.empty())
					connect(sig_y, sig_a);
				buf_norm_cell_queue.insert(cell);
				remove(cell);
				return;
			}

			if (sig.is_wire()) {
				Wire *w = sig.as_wire();
				if (direct_driven_wires.count(w))
					return;
				wire_queue_entries(w);
				if (w->driverCell_ != nullptr && w->driverCell_->getPort(w->driverPort_) != w) {
					log_abort();
				}

				if (w->driverCell_ == nullptr) {
					w->driverCell_ = cell;
					w->driverPort_ = port;
					direct_driven_wires.insert(w);
					return;
				}

				if (w->driverCell_ == cell && w->driverPort_ == port) {
					direct_driven_wires.insert(w);
					return;
				}
			}

			Wire *w = addWire(NEW_ID, GetSize(sig));
			wire_queue_entries(w);
			for (auto const &chunk : sig.chunks())
				if (chunk.wire)
					wire_queue_entries(chunk.wire);
			connect(w, sig);
			cell->setPort(port, w);
			direct_driven_wires.insert(w);
		};

		for (auto const &[cell, port_name] : buf_norm_cell_port_queue)
			enqueue_cell_port(cell, port_name);
		buf_norm_cell_port_queue.clear();

		for (auto wire : buf_norm_wire_queue)
			wire_queue_entries(wire);
		buf_norm_wire_queue.clear();

		for (auto &[a, b] : connections_)
			for (auto &sig : {a, b})
				for (auto const &chunk : sig.chunks())
					if (chunk.wire)
						wire_queue_entries(chunk.wire);

		while (wire_queue_pos < GetSize(wire_queue_entries)) {
			auto wire = wire_queue_entries[wire_queue_pos++];
			log("processing wire %s\n", log_id(wire));

			if (wire->driverCell_) {
				Cell *cell = wire->driverCell_;
				IdString port = wire->driverPort_;
				enqueue_cell_port(cell, port);
			}

			while (true) {
				auto found = buf_norm_connect_index.find(wire);
				if (found == buf_norm_connect_index.end())
					break;
				while (!found->second.empty()) {
					log("XXX B\n");
					Cell *connect_cell = *found->second.begin();
					log_assert(connect_cell->type == ID($connect));
					SigSpec const &sig_a = connect_cell->getPort(ID::A);
					SigSpec const &sig_b = connect_cell->getPort(ID::B);
					for (auto &side : {sig_a, sig_b})
						for (auto chunk : side.chunks())
							if (chunk.wire)
								wire_queue_entries(chunk.wire);
					connect(sig_a, sig_b);
					buf_norm_cell_queue.insert(connect_cell);
					remove(connect_cell);
				}
			}
		}


		SigMap sigmap;

		auto connections = connections_;
		new_connections({});

		for (auto const &[lhs, rhs] : connections) {
			log_assert(GetSize(lhs) == GetSize(rhs));
			for (int i = 0; i != GetSize(lhs); ++i) {
				SigBit a = sigmap(lhs[i]);
				SigBit b = sigmap(rhs[i]);
				// We won't ever merge two wires to be kept, where every public
				// wire or output port wire is considered a kept wire.

				// XXX is this still true?: We do want to merge an output port
				// wire that's fully connected to a public wire when there are
				// no other drivers involved, but we can't detect that at this
				// point, so we have to defer that merging.
				bool a_keep = a.wire == nullptr || keep_wire(a.wire) || a.wire->driverCell_ != nullptr;
				bool b_keep = b.wire == nullptr || keep_wire(b.wire) || b.wire->driverCell_ != nullptr;
				if (a_keep && b_keep)
					continue;
				sigmap.add(a, b);
				if (a_keep && !b_keep)
					sigmap.database.promote(a);
				if (b_keep && !a_keep)
					sigmap.database.promote(b);
			}
		}

		SigMap fully_connected;
		SigMap buf_connected;

		for (auto const &[lhs, rhs] : connections) {
			for (int i = 0; i != GetSize(lhs); ++i) {
				SigBit a = sigmap(lhs[i]);
				SigBit b = sigmap(rhs[i]);
				if (a == State::Sz || b == State::Sz)
					continue;
				fully_connected.add(a, b);
			}
		}

		for (auto wire : direct_driven_wires) {
			SigSpec z_mask;
			if (wire->driverCell_->type == ID($buf))
				z_mask = wire->driverCell_->getPort(ID::A);

			for (int i = 0; i != GetSize(wire); ++i) {
				SigBit net = fully_connected(sigmap(SigBit(wire, i)));
				if (!z_mask.empty() && z_mask[i] == State::Sz)
					continue;
				fully_connected.database.promote(net);
			}
		}

		dict<SigBit, PortBit> drivers;

		for (auto wire : direct_driven_wires) {
			SigSpec z_mask;
			if (wire->driverCell_->type == ID($buf))
				z_mask = wire->driverCell_->getPort(ID::A);

			for (int i = 0; i != GetSize(wire); ++i) {
				SigBit net = fully_connected(sigmap(SigBit(wire, i)));
				if (!z_mask.empty() && z_mask[i] == State::Sz)
					continue;


				auto [found, inserted] = drivers.emplace(net, PortBit(wire->driverCell_, wire->driverPort_, i));
				if (!inserted)
					found->second.cell = nullptr;
			}
		}

		for (auto [sb, pb] : drivers) {
			if (pb.cell)
				log("XXX sb %s driven by %s.%s[%d]\n", log_signal(sb), log_id(pb.cell), log_id(pb.port), pb.offset);
			else
				log("XXX sb %s driven by multiple drivers\n", log_signal(sb));
		}


		pool<pair<SigBit, SigBit>> undirected_connections;

		for (auto wire : wire_queue_entries) {
			bool direct = direct_driven_wires.count(wire);
			SigSpec wire_drivers;
			for (int i = 0; i < GetSize(wire); ++i) {
				SigBit bit(wire, i);
				SigBit mapped = fully_connected(sigmap(bit));

				if (!direct) {
					auto found_driver = drivers.find(mapped);
					if (found_driver != drivers.end() && found_driver->second.cell) {
						auto const &pb = found_driver->second;
						SigBit sb = pb.cell->getPort(pb.port)[pb.offset];
						wire_drivers.append(sb);
						buf_connected.add(mapped, sb);
						continue;
					} else {
						wire_drivers.append(State::Sz);
					}
				}
				if (bit < mapped)
					undirected_connections.emplace(bit, mapped);
				else if (mapped < bit)
					undirected_connections.emplace(mapped, bit);
			}
			if (!direct && wire_drivers != wire)
				addBuf(NEW_ID, wire_drivers, wire);
		}

		for (auto &[a, b] : undirected_connections) {
			buf_connected.apply(a);
			buf_connected.apply(b);
			buf_connected.add(a, b);
		}

		static auto sort_key = [](std::pair<SigBit, SigBit> const &p) {
			int first_offset = p.first.is_wire() ? p.first.offset : 0;
			int second_offset = p.second.is_wire() ? p.second.offset : 0;
			return std::make_tuple(p.first.wire, p.second.wire, first_offset - second_offset, p);
		};

		undirected_connections.sort([](std::pair<SigBit, SigBit> const &p, std::pair<SigBit, SigBit> const &q) {
			return sort_key(p) < sort_key(q);
		});

		SigSpec tmp_a, tmp_b;

		for (auto &[bit_a, bit_b] : undirected_connections) {
			tmp_a.append(bit_a);
			tmp_b.append(bit_b);
		}

		log("XXX A: %s\n", log_signal(tmp_a));
		log("XXX B: %s\n", log_signal(tmp_b));


		SigSpec sig_a, sig_b;
		SigBit next_a, next_b;

		auto emit_connect_cell = [&]() {
			if (sig_a.empty())
				return;
			Cell *connect_cell = addCell(NEW_ID, ID($connect));
			connect_cell->setParam(ID::WIDTH, GetSize(sig_a));
			connect_cell->setPort(ID::A, sig_a);
			connect_cell->setPort(ID::B, sig_b);
			sig_a = SigSpec();
			sig_b = SigSpec();
		};

		for (auto &[bit_a, bit_b] : undirected_connections) {
			if (bit_a == bit_b)
				continue;
			if (bit_a != next_a || bit_b != next_b)
				emit_connect_cell();

			sig_a.append(bit_a);
			sig_b.append(bit_b);
			next_a = bit_a;
			next_b = bit_b;
			if (next_a.is_wire())
				next_a.offset++;
			if (next_b.is_wire())
				next_b.offset++;

		}
		emit_connect_cell();

		buf_norm_cell_queue.clear();
	}

	for (auto cell : pending_deleted_cells) {
		delete cell;
	}
	pending_deleted_cells.clear();
}

void RTLIL::Cell::unsetPort(const RTLIL::IdString& portname)
{
	RTLIL::SigSpec signal;
	auto conn_it = connections_.find(portname);

	if (conn_it != connections_.end())
	{
		for (auto mon : module->monitors)
			mon->notify_connect(this, conn_it->first, conn_it->second, signal);

		if (module->design)
			for (auto mon : module->design->monitors)
				mon->notify_connect(this, conn_it->first, conn_it->second, signal);

		if (yosys_xtrace) {
			log("#X# Unconnect %s.%s.%s\n", log_id(this->module), log_id(this), log_id(portname));
			log_backtrace("-X- ", yosys_xtrace-1);
		}

		if (module->design && module->design->flagBufferedNormalized) {
			if (conn_it->second.is_wire()) {
				Wire *w = conn_it->second.as_wire();
				if (w->driverCell_ == this && w->driverPort_ == portname) {
					w->driverCell_ = nullptr;
					w->driverPort_ = IdString();
					module->buf_norm_wire_queue.insert(w);
				}
			}

			if (type == ID($connect)) {
				for (auto &[port, sig] : connections_) {
					for (auto &chunk : sig.chunks()) {
						if (!chunk.wire)
							continue;
						auto it = module->buf_norm_connect_index.find(chunk.wire);
						if (it == module->buf_norm_connect_index.end())
							continue;
						it->second.erase(this);
						if (it->second.empty())
							module->buf_norm_connect_index.erase(it);
					}
				}
				connections_.erase(conn_it);
				for (auto &[port, sig] : connections_) {
					for (auto &chunk : sig.chunks()) {
						if (!chunk.wire)
							continue;
						module->buf_norm_connect_index[chunk.wire].insert(this);
					}
				}
				return;
			}
		}

		connections_.erase(conn_it);
	}
}

void RTLIL::Cell::setPort(const RTLIL::IdString& portname, RTLIL::SigSpec signal)
{
	auto r = connections_.insert(portname);
	auto conn_it = r.first;
	if (!r.second && conn_it->second == signal)
		return;

	for (auto mon : module->monitors)
		mon->notify_connect(this, conn_it->first, conn_it->second, signal);

	if (module->design)
		for (auto mon : module->design->monitors)
			mon->notify_connect(this, conn_it->first, conn_it->second, signal);

	if (yosys_xtrace) {
		log("#X# Connect %s.%s.%s = %s (%d)\n", log_id(this->module), log_id(this), log_id(portname), log_signal(signal), GetSize(signal));
		log_backtrace("-X- ", yosys_xtrace-1);
	}

	if (module->design && module->design->flagBufferedNormalized)
	{
		// We eagerly clear a driver that got disconnected by changing this port connection
		if (conn_it->second.is_wire()) {
			Wire *w = conn_it->second.as_wire();
			if (w->driverCell_ == this && w->driverPort_ == portname) {
				w->driverCell_ = nullptr;
				w->driverPort_ = IdString();
				module->buf_norm_wire_queue.insert(w);
			}
		}

		auto dir = port_dir(portname);
		// This is a fast path that handles connecting a full driverless wire to an output port,
		// everything else is goes through the bufnorm queues and is handled during the next
		// bufNormalize call
		if (dir == RTLIL::PD_OUTPUT && signal.is_wire()) {
			Wire *w = signal.as_wire();
			if (w->driverCell_ == nullptr) {
				w->driverCell_ = this;
				w->driverPort_ = portname;

				conn_it->second = std::move(signal);
				return;
			}
		}

		if (dir == RTLIL::PD_OUTPUT) {
			module->buf_norm_cell_queue.insert(this);
			module->buf_norm_cell_port_queue.emplace(this, portname);
		} else {
			for (auto &chunk : signal.chunks())
				if (chunk.wire != nullptr && chunk.wire->driverCell_ == nullptr)
					module->buf_norm_wire_queue.insert(chunk.wire);
		}

		if (type == ID($connect)) {
			for (auto &[port, sig] : connections_) {
				for (auto &chunk : sig.chunks()) {
					if (!chunk.wire)
						continue;
					auto it = module->buf_norm_connect_index.find(chunk.wire);
					if (it == module->buf_norm_connect_index.end())
						continue;
					it->second.erase(this);
					if (it->second.empty())
						module->buf_norm_connect_index.erase(it);
				}
			}
			conn_it->second = std::move(signal);
			for (auto &[port, sig] : connections_) {
				for (auto &chunk : sig.chunks()) {
					if (!chunk.wire)
						continue;
					module->buf_norm_connect_index[chunk.wire].insert(this);
				}
			}
			return;
		}
	}
	conn_it->second = std::move(signal);

}

YOSYS_NAMESPACE_END
