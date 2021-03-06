`timescale 1ns / 1ps

//////////////////////////////////////////////////////////////////////////////////
// Company: Università della Svizzera italiana
// Engineer: Pietro Bressana
// 
// Create Date: 04/08/2016
// Module Name: tuser_in_fsm
// Project Name: SDNet
//////////////////////////////////////////////////////////////////////////////////

module tuser_in_fsm (

//#################################
//####       INTERFACES
//#################################

// CLK & RST
tin_aclk,
tin_arst,

// AXIS INPUT INTERFACE
tin_avalid,
tin_aready,
tin_adata,
tin_akeep,
tin_atlast,
tin_atuser,

// AXIS OUTPUT INTERFACE
tin_bvalid,
tin_bready,
tin_bdata,
tin_bkeep,
tin_btlast,

// TUPLE OUTPUT INTERFACE
tin_valid,
tin_data,

// DEBUG PORTS
dbg_state

);

//######################################
//####       TYPE OF INTERFACES
//######################################

// CLK & RST
input     [0:0]                   tin_aclk ;
input     [0:0]                   tin_arst ;

// AXIS INPUT INTERFACE
input     [0:0]                   tin_avalid ;
output  reg    [0:0]                   tin_aready ;
input     [255:0]                 tin_adata ;
input     [31:0]                  tin_akeep ;
input     [0:0]                   tin_atlast ;
input     [127:0]                 tin_atuser ;

// AXIS OUTPUT INTERFACE
output  reg    [0:0]                   tin_bvalid ;
input     [0:0]                   tin_bready ;
output  reg    [255:0]                 tin_bdata ;
output  reg    [31:0]                  tin_bkeep ;
output  reg    [0:0]                   tin_btlast ;

// TUPLE OUTPUT INTERFACE
output reg  [0:0]                 tin_valid ;
output reg  [127:0]               tin_data ;

// DEBUG PORTS
output      [0:2]                   dbg_state;

//#################################
//####     WIRES & REGISTERS
//#################################

// FSM STATES
reg     [0:2]                   state = 3'bxxx ;
// 000: IDLE
// 001: WAIT
// 010: GO

// DEBUG
assign    dbg_state = state ;

//#################################
//####   FINITE STATE MACHINE
//#################################

always @ ( posedge tin_aclk )

 if (tin_arst == 1)
 
     ////////////////////////// 
     //        RESET
     ////////////////////////// 
     begin

       tin_aready <= 0;

       tin_bvalid <= 0;
       tin_bdata <= 0;
       tin_bkeep <= 0;
       tin_btlast <= 0;
    
       tin_valid <= 0;
       tin_data <= 0;
       
       state <= 3'b000; // IDLE
    
     end

 else begin // begin #2

   case(state)

    ////////////////////////// 
    //    STATE S000: IDLE
    ////////////////////////// 
      3'b000 : begin

          if( tin_avalid == 1 )

          begin // IDLE ==> WAIT

           tin_aready <= 0;

           tin_bvalid <= 1;
           tin_bdata <= tin_adata;
           tin_bkeep <= tin_akeep;
           tin_btlast <= 0;
        
           tin_valid <= 0;
           tin_data <= tin_atuser;
   
            state <= 3'b001; // WAIT

          end

          else

          begin // IDLE ==> IDLE

           tin_aready <= 0;

           tin_bvalid <= 0;
           tin_bdata <= 0;
           tin_bkeep <= 0;
           tin_btlast <= 0;
        
           tin_valid <= 0;
           tin_data <= 0;
   
            state <= 3'b000; // IDLE

          end

           end

    ////////////////////////// 
    //    STATE S001: WAIT
    ////////////////////////// 
       3'b001 : begin

           if( tin_bready == 1 )

           begin // WAIT ==> IDLE

           tin_aready <= 1;

           tin_bvalid <= 1;
           tin_bdata <= tin_adata;
           tin_bkeep <= tin_akeep;
           tin_btlast <= 0;
        
           tin_valid <= 1;
           tin_data <= tin_atuser;
       
          state <= 3'b010; // GO

           end

           else

           begin // WAIT ==> WAIT
                     
            tin_aready <= 0;

            tin_bvalid <= 1;
            tin_bdata <= tin_adata;
            tin_bkeep <= tin_akeep;
            tin_btlast <= 0;
            
            tin_valid <= 0;
            tin_data <= tin_atuser;
       
             state <= 3'b001; // WAIT

           end

            end

      ////////////////////////// 
      //    STATE S010: GO
      ////////////////////////// 
      3'b010 : begin

             if( tin_atlast == 1)

             begin // GO ==> IDLE

             tin_aready <= 0;

             tin_bvalid <= 0;
             tin_bdata <= tin_adata;
             tin_bkeep <= tin_akeep;
             tin_btlast <= 1;
          
             tin_valid <= 0;
             tin_data <= tin_atuser;
         
            state <= 3'b000; // IDLE

             end

             else

             begin // GO ==> GO
                       
              tin_aready <= 1;

              tin_bvalid <= 1;
              tin_bdata <= tin_adata;
              tin_bkeep <= tin_akeep;
              tin_btlast <= 0;
              
              tin_valid <= 0;
              tin_data <= tin_atuser;
         
               state <= 3'b010; // GO

             end

              end


   endcase

 end // begin #2

endmodule // tuser_in_fsm