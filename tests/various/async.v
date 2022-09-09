`define MAXQ 3
module uut (
	input clk,
	input d, ad, r, e,
	output [`MAXQ:0] q
);
	reg q0;
	always @(posedge clk) begin
		if (r)
			q0 <= 0;
		else if (e)
			q0 <= d;
	end

	reg q1;
	always @(posedge clk, posedge r) begin
		if (r)
			q1 <= 0;
		else if (e)
			q1 <= d;
	end

	reg q2;
	always @(posedge clk, negedge r) begin
		if (!r)
			q2 <= 0;
		else if (!e)
			q2 <= d;
	end


	wire q3;

// `ifdef TESTBENCH
// 	always @(posedge clk, negedge r) begin
// 		if (r)
// 			q3 <= ad;
// 		else if (!e)
// 			q3 <= d;
// 	end
// `else
	\$aldffe #(.WIDTH(1), .CLK_POLARITY(1'b1), .EN_POLARITY(1'b1), .ALOAD_POLARITY(1'b1)) aldff (.CLK(clk), .ALOAD(r), .AD(ad), .EN(e), .D(d), .Q(q3));
// `endif

	assign q = {q3, q2, q1, q0};
endmodule




`ifdef TESTBENCH

// module \$aldff (CLK, ALOAD, AD, D, Q);

//     parameter WIDTH = 0;
//     parameter CLK_POLARITY = 1'b1;
//     parameter ALOAD_POLARITY = 1'b1;

//     input CLK, ALOAD;
//     input [WIDTH-1:0] AD;
//     input [WIDTH-1:0] D;
//     output [WIDTH-1:0] Q;
//     wire pos_clk = CLK == CLK_POLARITY;
//     wire pos_aload = ALOAD == ALOAD_POLARITY;


//    	// a latch to store the async-load value
//     reg [WIDTH-1:0] latched_rval;
//     always @*
//         if (pos_aload) latched_rval = AD;

//     // a regular FF to store the clocked data value
//     reg [WIDTH-1:0] q_without_reset;
//     always @(posedge pos_clk)
//         q_without_reset <= D;

//     // an asynchronous-reset flip-flop to remember last event
//     reg last_event_was_clock;
//     always @(posedge pos_clk or posedge pos_aload)
//         if (pos_aload) last_event_was_clock <= 0;
//         else      last_event_was_clock <= 1;

//     // output either the latched reset value or clocked data value
//     assign Q = last_event_was_clock ? q_without_reset : latched_rval;


// endmodule


module \$aldffe (CLK, ALOAD, AD, EN, D, Q);

    parameter WIDTH = 0;
    parameter CLK_POLARITY = 1'b1;
    parameter EN_POLARITY = 1'b1;
    parameter ALOAD_POLARITY = 1'b1;

    input CLK, ALOAD, EN;
    input [WIDTH-1:0] AD;
    input [WIDTH-1:0] D;
    output [WIDTH-1:0] Q;
    wire pos_clk = CLK == CLK_POLARITY;
    wire pos_aload = ALOAD == ALOAD_POLARITY;


   	// // a latch to store the async-load value
    // reg [WIDTH-1:0] latched_rval;
    // always @*
    //     if (pos_aload) latched_rval = AD;

    // // a regular FF to store the clocked data value
    // reg [WIDTH-1:0] q_without_reset;
    // always @(posedge pos_clk)
    //     q_without_reset <= D;

    // // an asynchronous-reset flip-flop to remember last event
    // reg last_event_was_clock;
    // always @(posedge pos_clk or posedge pos_aload)
    //     if (pos_aload) last_event_was_clock <= 0;
    //     else      last_event_was_clock <= 1;

    // // output either the latched reset value or clocked data value
    // assign Q = last_event_was_clock ? q_without_reset : latched_rval;

	aldffe_bit bit_0 (.CLK(pos_clk), .ALOAD(pos_aload), .AD(AD[0]), .EN(EN), .D(D[0]), .Q(Q[0]));

endmodule

primitive aldffe_bit (Q, CLK, ALOAD, AD, EN, D);
	input CLK;
	input ALOAD;
	input AD;
	input EN;
	input D;

	output Q;
	reg Q;

	table
		? 1 1 ? ? : ? : 1;
		? 1 0 ? ? : ? : 0;
		p ? ? 1 1 : ? : 1;
		p ? ? 1 0 : ? : 0;
		* ? ? ? ? : ? : -;
		? * ? ? ? : ? : -;
		? ? * ? ? : ? : -;
		? ? ? * ? : ? : -;
		? ? ? ? * : ? : -;
	endtable
endprimitive


module \$ff #(
	parameter integer WIDTH = 1
) (
	input [WIDTH-1:0] D,
	output reg [WIDTH-1:0] Q
);
	wire sysclk = testbench.sysclk;
	always @(posedge sysclk)
		Q <= D;
endmodule

module testbench;
	reg sysclk;
	always #5 sysclk = (sysclk === 1'b0);

	reg clk;
	always @(posedge sysclk) clk = (clk === 1'b0);

	reg d, ad, r, e;

	wire [`MAXQ:0] q_uut;
	uut uut (.clk(clk), .d(d), .ad(ad), .r(r), .e(e), .q(q_uut));

	wire [`MAXQ:0] q_syn;
	syn syn (.clk(clk), .d(d), .ad(ad), .r(r), .e(e), .q(q_syn));

	wire [`MAXQ:0] q_prp;
	prp prp (.clk(clk), .d(d), .ad(ad), .r(r), .e(e), .q(q_prp));

	wire [`MAXQ:0] q_a2s;
	a2s a2s (.clk(clk), .d(d), .ad(ad), .r(r), .e(e), .q(q_a2s));

	wire [`MAXQ:0] q_ffl;
	ffl ffl (.clk(clk), .d(d), .ad(ad), .r(r), .e(e), .q(q_ffl));


	task printq;
		reg [5*8-1:0] msg;
		begin
			msg = "OK";
			if (q_uut !== q_prp) msg = "PRP";
			if (q_uut !== q_a2s) msg = "A2S";
			if (q_uut !== q_ffl) msg = "FFL";
			if (q_uut !== q_syn) msg = "SYN";
			$display("%6t %b %b %b %b %b %s", $time, q_uut, q_syn, q_prp, q_a2s, q_ffl, msg);
			//if (msg != "OK") $finish;
		end
	endtask

	initial if(1) begin
		$dumpfile("async.vcd");
		$dumpvars(0, testbench);
	end

	initial begin
		@(posedge clk);

		d <= 0;
		ad <= 0;
		r <= 0;
		e <= 0;
		@(posedge clk);

		e <= 1;
		@(posedge clk);

		e <= 0;
		repeat (1000) begin
			@(posedge clk);

			printq;
			d <= $random;

			ad <= $random;
			e <= $random;
			r <= $random;
		end
		$display("PASS");
		$finish;
	end
endmodule
`endif
