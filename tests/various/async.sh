#!/bin/bash
set -ex
../../yosys -q -o async_syn.v -p 'read_verilog -icells async.v; hierarchy -top uut' -p 'synth; rename uut syn'
../../yosys -q -o async_prp.v -p 'read_verilog -icells async.v; hierarchy -top uut' -p 'prep; rename uut prp'
../../yosys -q -o async_a2s.v -p 'read_verilog -icells async.v; hierarchy -top uut' -p 'prep; async2sync -neghold; rename uut a2s'
../../yosys -q -o async_ffl.v -p 'read_verilog -icells async.v; hierarchy -top uut' -p 'prep; clk2fflogic -neghold; rename uut ffl'
iverilog -o async_sim -DTESTBENCH async.v async_???.v
vvp -N async_sim > async.out
tail async.out
grep PASS async.out
rm -f async_???.v async_sim #async.out async.vcd
