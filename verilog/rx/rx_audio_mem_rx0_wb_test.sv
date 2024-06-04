
`timescale 10ns / 10ns

module rx_audio_mem_rx0_wb_test (

    );
    
    reg adc_clk;
    localparam ADC_CLK_PERIOD = 1;
    initial adc_clk = 1'b1;
    always # (ADC_CLK_PERIOD/2.0)
        adc_clk = ~adc_clk;

    reg [15:0] nrx_samps_A;
    reg rx_avail_A, rx_avail_wb_A;
    reg [15:0] rx_data_A = 16'b0;
    reg [47:0] ticks_latched_A = 48'b0;
    
    wire ser, rd_getI, rd_getQ, rd_getWB, awb_debug;
    wire [2:0] rxn_o;
    
    reg cpu_clk;
    localparam CPU_CLK_PERIOD = 1;
    initial cpu_clk = 1'b1;
    always # (CPU_CLK_PERIOD/2.0)
        cpu_clk = ~cpu_clk;

    reg get_rx_srq_C, get_rx_samp_C, reset_bufs_C, get_buf_ctr_C;
    
    wire rx_rd_C;
    wire [15:0] rx_dout_C;

    localparam WB_CYCLES = 6;
    localparam WB_ONLY_CYCLES = (WB_CYCLES-1);
    localparam ALL_CYCLES = (WB_CYCLES+1);

//`define SHORT
`ifdef SHORT
    localparam N_SAMPS = 16'd7;
    localparam CYCLES = 3;
`else
    localparam N_SAMPS = 16'd679;           // 679 (0x2a7) = 97*(1+6)
    localparam CYCLES = (679/ALL_CYCLES+3); // 679/7 + ceil/slop
`endif

    integer i, j;
    initial begin
        #2;
        #2; reset_bufs_C = 1;
        #2; reset_bufs_C = 0;
        #10; nrx_samps_A = N_SAMPS;
        
        for (i = 0; i < CYCLES; i++)
        begin
            #(2); rx_avail_wb_A = 1; rx_avail_A = 1;
            #(2); rx_avail_wb_A = 0; rx_avail_A = 0;
            #(30);
            for (j = 0; j < WB_ONLY_CYCLES; j++)
            begin
                #(2); rx_avail_wb_A = 1;
                #(2); rx_avail_wb_A = 0;
                #(30);
            end
        end
    end
    
    rx_audio_mem_rx0_wb rx_audio_mem_wb_inst (
		.adc_clk		(adc_clk),
		.nrx_samps      (nrx_samps_A),
		.rx_avail_A     (rx_avail_A),
		.rx_avail_wb_A  (rx_avail_wb_A),
		.rx_din_A       (rx_data_A),
		.ticks_A        (ticks_latched_A),
		// o
		.ser            (ser),
		.rd_getI        (rd_getI),
		.rd_getQ        (rd_getQ),
		.rd_getWB       (rd_getWB),
		.rxn_o          (rxn_o),
		.debug          (awb_debug),

		.cpu_clk        (cpu_clk),
		.get_rx_srq_C   (get_rx_srq_C),
		.get_rx_samp_C  (get_rx_samp_C),
		.reset_bufs_C   (reset_bufs_C),
		.get_buf_ctr_C  (get_buf_ctr_C),
		// o
		.rx_rd_C        (rx_rd_C),
		.rx_dout_C      (rx_dout_C)
    );

endmodule
