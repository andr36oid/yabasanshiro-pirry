/* Fast path of the SCU DSP interpreter (generated from scu.c; do not edit by hand).
   Interpreter state is kept in locals instead of memory: incFlg as a bit mask `inc`, flags Z/S/C in a local
   copy `pcp` (written back once per instruction), ALU in a local copy `alu`. The breakpoint loop is omitted
   (the caller uses the generic path when breakpoints are set); writed1busdest/writeloadimdest/readgensrc are
   inlined. The arithmetic expressions are identical to the generic path. */
static inline __attribute__((always_inline)) u32 readgensrc_r(u8 num, u32 *inc)
{
   u32 val;

   if( num <= 7  ){
     *inc |= (u32)((num >> 2) & 0x01) << (num & 0x3);
     // Finish Previous DMA operation
     if (ScuDsp->dsp_dma_wait > 0) {
       ScuDsp->dsp_dma_wait = 0;
       step_dsp_dma(ScuDsp);
     }
     //LOG("readgensrc from [%d][%d]= %08X", (num & 0x3), ScuDsp->CT[(num & 0x3)] & 0x3F, ScuDsp->MD[(num & 0x3)][ScuDsp->CT[(num & 0x3)] & 0x3F]);
     return ScuDsp->MD[(num & 0x3)][ScuDsp->CT[(num & 0x3)]&0x3F];
   }else{
     if (num == 0x9)  // ALL
       return (u32)ScuDsp->ALU.part.L;
     else if (num == 0xA) // ALH
       return (u32)(ScuDsp->ALU.all >> 16); ////(u32)((ScuDsp->ALU.all & (u64)(0x0000ffffffff0000))  >> 16);
   }
   return 0xFFFFFFFF;
}

static inline __attribute__((always_inline)) void writed1busdest_r(u8 num, u32 val, u32 *inc)
{
  //LOG("writed1busdest [%d][%d] = %08X",num, ScuDsp->CT[0] & 0x3F, val);

  // Finish Previous DMA operation
  if (ScuDsp->dsp_dma_wait > 0) {
    ScuDsp->dsp_dma_wait = 0;
    step_dsp_dma(ScuDsp);
  }

   switch(num) { 
      case 0x0:
          ScuDsp->MD[0][ScuDsp->CT[0]&0x3F] = val;
          *inc |= 1u;
          return;
      case 0x1:
        ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F] = val;
          *inc |= 2u;
          return;
      case 0x2:
        ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F] = val;
          *inc |= 4u;
          return;
      case 0x3:
        ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F] = val;
          *inc |= 8u;
          return;
      case 0x4:
          ScuDsp->RX = val;
          return;
      case 0x5:
          ScuDsp->P.all = (signed)val;
          return;
      case 0x6:
          ScuDsp->RA0 = val;
          return;
      case 0x7:
          ScuDsp->WA0 = val;
          return;
      case 0xA:
          ScuDsp->LOP = (u16)val;
          return;
      case 0xB:
          ScuDsp->TOP = (u8)val;
          return;
      case 0xC:
          ScuDsp->CT[0] = (u8)val;
          *inc &= ~1u;
          return;
      case 0xD:
          ScuDsp->CT[1] = (u8)val;
          *inc &= ~2u;
          return;
      case 0xE:
          ScuDsp->CT[2] = (u8)val;
          *inc &= ~4u;
          return;
      case 0xF:
          ScuDsp->CT[3] = (u8)val;
          *inc &= ~8u;
          return;
      default: break;
   }
}

static inline __attribute__((always_inline)) void writeloadimdest_r(u8 num, u32 val, u32 *inc)
{
  // Finish Previous DMA operation
  if (ScuDsp->dsp_dma_wait > 0) {
    ScuDsp->dsp_dma_wait = 0;
    step_dsp_dma(ScuDsp);
  }

   switch(num) { 
      case 0x0: // MC0
        //LOG("writeloadimdest [%d][%d] = %08X", num, ScuDsp->CT[0] & 0x3F, val);
        ScuDsp->MD[0][ScuDsp->CT[0] & 0x3F] = val;
          *inc |= 1u;
          return;
      case 0x1: // MC1
        //LOG("writeloadimdest [%d][%d] = %08X", num, ScuDsp->CT[1] & 0x3F, val);
        ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F] = val;
        *inc |= 2u;
        return;
      case 0x2: // MC2
        //LOG("writeloadimdest [%d][%d] = %08X", num, ScuDsp->CT[2] & 0x3F, val);
        ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F] = val;
          *inc |= 4u;
          return;
      case 0x3: // MC3
        //LOG("writeloadimdest [%d][%d] = %08X", num, ScuDsp->CT[3] & 0x3F, val);
        ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F] = val;
          *inc |= 8u;
          return;
      case 0x4: // RX
          ScuDsp->RX = val;
          return;
      case 0x5: // PL
          ScuDsp->P.all = (s32)val;
          return;
      case 0x6: // RA0
          val = (val & 0x1FFFFFF);
          ScuDsp->RA0 = val;
          return;
      case 0x7: // WA0
          val = (val & 0x1FFFFFF);
          ScuDsp->WA0 = val;
          return;
      case 0xA: // LOP
          ScuDsp->LOP = (u16)(val & 0x0FFF);
          return;
      case 0xC: // PC->TOP, PC
          ScuDsp->TOP = ScuDsp->PC+1;
          ScuDsp->jmpaddr = val;
          ScuDsp->delayed = 0;
          return;
      default: 
        LOG("writeloadimdest BAD NUM %d,%d",num,val);
        break;
   }
}

static unsigned long long s_dr_bef = 0, s_dr_auf = 0;
static time_t s_dr_t = 0;
static void __attribute__((noinline)) ScuDspExecReg(u32 timing) {
   u32 inc = 0;                                   /* incFlg as bit mask */
   unsigned long long n_bef = 0;
   __typeof__(ScuDsp->ALU) alu;
   __typeof__(ScuDsp->ProgControlPort) pcp;
     s32 dsp_counter = (s32)timing;
      while (dsp_counter > 0) {
         u32 instruction;

         if (ScuDsp->ProgControlPort.part.T0 != 0) {
           step_dsp_dma(ScuDsp);
         }

         instruction = ScuDsp->ProgramRam[ScuDsp->PC];
         n_bef++;
         //LOG("scu: dsp %08X @ %08X", instruction, ScuDsp->PC);
         inc = 0;

         alu.all = ScuDsp->AC.all;                    /* local ALU */
         pcp.all = ScuDsp->ProgControlPort.all;       /* local flags */

         // ALU commands
         switch (instruction >> 26)
         {
            case 0x0: // NOP
               //AC is moved as-is to the ALU
              //alu.all = ScuDsp->AC.all;
               break;
            case 0x1: // AND
               //the upper 16 bits of AC are not modified for and, or, add, sub, rr and rl8
              alu.part.L = (s64)((u32)ScuDsp->AC.part.L & (u32)ScuDsp->P.part.L);

               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if ((s64)alu.part.L < 0)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               pcp.part.C = 0;
               break;
            case 0x2: // OR
              alu.part.L = (u64)((u32)ScuDsp->AC.part.L | (u32)ScuDsp->P.part.L);

               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if ((s64)alu.part.L < 0)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               pcp.part.C = 0;
               break;
            case 0x3: // XOR
              alu.part.L = (u64)((u32)ScuDsp->AC.part.L ^ (u32)ScuDsp->P.part.L);

               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if ((s64)alu.part.L < 0)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               pcp.part.C = 0;
               break;
            case 0x4: // ADD
               alu.part.L = (s32)ScuDsp->AC.part.L + (s32)ScuDsp->P.part.L;
               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if ((s32)alu.part.L < 0)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               //0x00000001 + 0xFFFFFFFF will set the carry bit, needs to be unsigned math
               if (((u64)(u32)ScuDsp->P.part.L + (u64)(u32)ScuDsp->AC.part.L) & 0x100000000){
                 pcp.part.C = 1;
               }
               else{
                 pcp.part.C = 0;
               }

 
               //if (alu.part.L ??) // set overflow flag
               //    pcp.part.V = 1;
               //else
               //   pcp.part.V = 0;
               break;
            case 0x5: // SUB
            {
              //u64 ans = (u64)ScuDsp->AC.part.L - (u32)ScuDsp->P.part.L;
              alu.part.L = (s32)ScuDsp->AC.part.L - (s32)ScuDsp->P.part.L;
              //pcp.part.C = ((ans >> 32) & 0x01);

              //alu.part.L = ans;

              if (alu.part.L == 0)
                pcp.part.Z = 1;
              else
                pcp.part.Z = 0;

              if ((s64)alu.part.L < 0)
                pcp.part.S = 1;
              else
                pcp.part.S = 0;

              //0x00000001 - 0xFFFFFFFF will set the carry bit, needs to be unsigned math
              if ((((u64)(u32)ScuDsp->AC.part.L - (u64)(u32)ScuDsp->P.part.L)) & 0x100000000)
                pcp.part.C = 1;
              else
                pcp.part.C = 0;

              //0x00000001 - 0xFFFFFFFF will set the carry bit, needs to be unsigned math
              //if ((((u64)(u32)ScuDsp->AC.part.L - (u64)(u32)ScuDsp->P.part.L)) & 0x100000000)
              //  pcp.part.C = 1;
              //else
              //  pcp.part.C = 0;


              //               if (alu.part.L ??) // set overflow flag
              //                  pcp.part.V = 1;
              //               else
              //                  pcp.part.V = 0;
            }
               break;
            case 0x6: // AD2
              alu.all = (s64)ScuDsp->AC.all +(s64)ScuDsp->P.all;
               if (alu.all == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               //0x500000000000 + 0xd00000000000 will set the sign bit
               if (alu.all & 0x800000000000)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               //AC.all and P.all are sign-extended so we need to mask it off and check for a carry
               if (((ScuDsp->AC.all & 0xffffffffffff) + (ScuDsp->P.all & 0xffffffffffff)) & (0x1000000000000))
                  pcp.part.C = 1;
               else
                  pcp.part.C = 0;

//               if (alu.part.unused != 0)
//                  pcp.part.V = 1;
//               else
//                  pcp.part.V = 0;

               break;
            case 0x8: // SR
              pcp.part.C = ScuDsp->AC.part.L & 0x1;
               alu.part.L = (ScuDsp->AC.part.L & 0x80000000) | (ScuDsp->AC.part.L >> 1);

               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if (alu.part.L & 0x80000000)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               //0x00000001 >> 1 will set the carry bit
               //pcp.part.C = alu.part.L >> 31; would not handle this case
               break;
            case 0x9: // RR
              pcp.part.C = ScuDsp->AC.part.L & 0x1;
               alu.part.L = ((u32)(pcp.part.C) << 31) | ((u32)(ScuDsp->AC.part.L) >> 1) ;
               
               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               //rotating 0x00000001 right will produce 0x80000000 and set 
               //the sign bit.
               if (alu.part.L & 0x80000000)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;
               break;
            case 0xA: // SL
              pcp.part.C = (ScuDsp->AC.part.L >> 31) & 0x01;

               alu.part.L = (u32)(ScuDsp->AC.part.L << 1);

               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               if (alu.part.L & 0x80000000)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;
               break;
            case 0xB: // RL

              pcp.part.C = (ScuDsp->AC.part.L >> 31) & 0x01;

               alu.part.L = (((u32)ScuDsp->AC.part.L << 1) | pcp.part.C);
               
               if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;
         
               if (alu.part.L & 0x80000000)
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;
               
               //ScuDsp->AC.part.L = alu.part.L;
               break;
            case 0xF: // RL8

              pcp.part.C = (ScuDsp->AC.part.L >> 24) & 0x01;
              alu.part.L  = ((u32)(ScuDsp->AC.part.L << 8) | ((ScuDsp->AC.part.L >> 24) & 0xFF)) ;

              if (alu.part.L == 0)
                  pcp.part.Z = 1;
               else
                  pcp.part.Z = 0;

               //rotating 0x00ffffff left 8 will produce 0xffffff00 and
               //set the sign bit
               if ( alu.part.L & 0x80000000 )
                  pcp.part.S = 1;
               else
                  pcp.part.S = 0;

               //rotating 0xff000000 left 8 will produce 0x000000ff and set the
               //carry bit
               //pcp.part.C = (ScuDsp->AC.part.L >> 24) & 0x01;
               break;
            default: break;
         }

         ScuDsp->ALU.all = alu.all;                    /* single write-back */
         ScuDsp->ProgControlPort.all = pcp.all;

         switch (instruction >> 30) {
         case 0x00: // Operation Commands
               switch ((instruction >> 23) & 0x3)
               {
                  case 2: // MOV MUL, P
                    ScuDsp->P.all = (s64)ScuDsp->RX * (s32)ScuDsp->RY; // ScuDsp->MUL.all;
                     break;
                  case 3: // MOV [s], P
                     //s32 cast to sign extend
                    ScuDsp->P.all = (s64)(s32)readgensrc_r((instruction >> 20) & 0x7, &inc);
                     break;
                  default: break;
               }
               // X-bus
               if ((instruction >> 23) & 0x4)
               {
                 // MOV [s], X
                 ScuDsp->RX = readgensrc_r((instruction >> 20) & 0x7, &inc);
               }

               // Y-bus
               if ((instruction >> 17) & 0x4) 
               {
                  // MOV [s], Y
                  ScuDsp->RY = readgensrc_r((instruction >> 14) & 0x7, &inc);
               }
               switch ((instruction >> 17) & 0x3)
               {
                  case 1: // CLR A
                     ScuDsp->AC.all = 0;
                     break;
                  case 2: // MOV ALU,A
                     ScuDsp->AC.all = alu.all;
                     break;
                  case 3: // MOV [s],A
                     //s32 cast to sign extend
                     ScuDsp->AC.all = (s64)(s32)readgensrc_r((instruction >> 14) & 0x7, &inc);
                     break;
                  default: break;
               }

   
               // D1-bus
               switch ((instruction >> 12) & 0x3)
               {
                  case 1: // MOV SImm,[d]
                    if (inc & 1u){ ScuDsp->CT[0]++; ScuDsp->CT[0] &= 0x3f; inc &= ~1u; };
                    if (inc & 2u){ ScuDsp->CT[1]++; ScuDsp->CT[1] &= 0x3f; inc &= ~2u; };
                    if (inc & 4u){ ScuDsp->CT[2]++; ScuDsp->CT[2] &= 0x3f; inc &= ~4u; };
                    if (inc & 8u){ ScuDsp->CT[3]++; ScuDsp->CT[3] &= 0x3f; inc &= ~8u; };
                     writed1busdest_r((instruction >> 8) & 0xF, (u32)(signed char)(instruction & 0xFF), &inc);
                     break;
                  case 3: // MOV [s],[d]
                     writed1busdest_r((instruction >> 8) & 0xF, readgensrc_r(instruction & 0xF, &inc), &inc);
                     break;
                  default: break;
               }

               break;
            case 0x02: // Load Immediate Commands
               if ((instruction >> 25) & 1)
               {
                  switch ((instruction >> 19) & 0x3F) {
                     case 0x01: // MVI Imm,[d]NZ
                        if (!ScuDsp->ProgControlPort.part.Z)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x02: // MVI Imm,[d]NS
                        if (!ScuDsp->ProgControlPort.part.S)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x03: // MVI Imm,[d]NZS
                        if ( ScuDsp->ProgControlPort.part.Z == 0 && ScuDsp->ProgControlPort.part.S == 0)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x04: // MVI Imm,[d]NC
                        if (!ScuDsp->ProgControlPort.part.C)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x08: // MVI Imm,[d]NT0
                        if (!ScuDsp->ProgControlPort.part.T0)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x21: // MVI Imm,[d]Z
                        if (ScuDsp->ProgControlPort.part.Z)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x22: // MVI Imm,[d]S
                        if (ScuDsp->ProgControlPort.part.S)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x23: // MVI Imm,[d]ZS
                        if (ScuDsp->ProgControlPort.part.Z || ScuDsp->ProgControlPort.part.S)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x24: // MVI Imm,[d]C
                        if (ScuDsp->ProgControlPort.part.C)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     case 0x28: // MVI Imm,[d]T0
                        if (ScuDsp->ProgControlPort.part.T0)
                           writeloadimdest_r((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | ((instruction & 0x40000) ? 0xFFF80000 : 0x00000000), &inc);
                        break;
                     default: break;
                  }
               }
               else
               {
                  // MVI Imm,[d]
                  int value = (instruction & 0x1FFFFFF);
                  if (value & 0x1000000) value |= 0xfe000000;
                  writeloadimdest_r((instruction >> 26) & 0xF, value, &inc);
                }
               break;
            case 0x03: // Other
            {
               switch((instruction >> 28) & 0xF) {
                 case 0x0C: // DMA Commands
                 {
                   // Finish Previous DMA operation
                   if (ScuDsp->dsp_dma_wait > 0) {
                     ScuDsp->dsp_dma_wait = 0;
                     step_dsp_dma(ScuDsp);
                   }

                   ScuDsp->dsp_dma_instruction = instruction;
                   ScuDsp->ProgControlPort.part.T0 = 1;

                   int Counter = 0;
                   if ( ((instruction >> 10) & 0x1F) == 0x00 || 
                        ((instruction >> 10) & 0x1F) == 0x04  || 
                        ((instruction >> 11) & 0x0F) == 0x08 || 
                        ((instruction >> 10) & 0x1F) == 0x14 )
                   {
                      Counter = instruction & 0xFF;
                   }
                   else if (
                     ((instruction >> 11) & 0x0F) == 0x04 || 
                     ((instruction >> 10) & 0x1F) == 0x0C || 
                     ((instruction >> 11) & 0x0F) == 0x0C || 
                     ((instruction >> 10) & 0x1F) == 0x1C)
                   {
                     switch ((instruction & 0x7))
                     {
                     case 0x00: Counter = ScuDsp->MD[0][ScuDsp->CT[0] & 0x3F]; break;
                     case 0x01: Counter = ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F]; break;
                     case 0x02: Counter = ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F]; break;
                     case 0x03: Counter = ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F]; break;
                     case 0x04: Counter = ScuDsp->MD[0][ScuDsp->CT[0] & 0x3F]; ScuDsp->CT[0]++; ScuDsp->CT[0] &= 0x3F; break;
                     case 0x05: Counter = ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F]; ScuDsp->CT[1]++; ScuDsp->CT[1] &= 0x3F; break;
                     case 0x06: Counter = ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F]; ScuDsp->CT[2]++; ScuDsp->CT[2] &= 0x3F; break;
                     case 0x07: Counter = ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F]; ScuDsp->CT[3]++; ScuDsp->CT[3] &= 0x3F; break;
                     }

                   }

                   ScuDsp->dsp_dma_size = Counter;
                   ScuDsp->dsp_dma_wait = Counter >> 12; // DMA operation will be start when this count is zero
                   ScuDsp->WA0M = ScuDsp->WA0;
                   ScuDsp->RA0M = ScuDsp->RA0;
                   //LOG("Start DSP DMA RA=%08X WA=%08X inst=%08X count=%d wait = %d", ScuDsp->RA0M, ScuDsp->WA0M, ScuDsp->dsp_dma_instruction, Counter, ScuDsp->dsp_dma_wait );
                   break;
                  }
                  case 0x0D: // Jump Commands
                    if (ScuDsp->jmpaddr != 0xffffffff) {
                      break;
                    }
                     switch ((instruction >> 19) & 0x7F) {
                        case 0x00: // JMP Imm
                           ScuDsp->jmpaddr = instruction & 0xFF;
                           ScuDsp->delayed = 0;
                           break;
                        case 0x41: // JMP NZ, Imm
                           if (!ScuDsp->ProgControlPort.part.Z)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }
                           break;
                        case 0x42: // JMP NS, Imm
                           if (!ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }

                           //LOG("scu\t: JMP NS: S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x43: // JMP NZS, Imm
                           if ( ScuDsp->ProgControlPort.part.Z==0 && ScuDsp->ProgControlPort.part.S == 0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }

                           //LOG("scu\t: JMP NZS: Z = %d, S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.Z, (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x44: // JMP NC, Imm
                           if (!ScuDsp->ProgControlPort.part.C)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }
                           break;
                        case 0x48: // JMP NT0, Imm
                           if (!ScuDsp->ProgControlPort.part.T0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }

                           //LOG("scu\t: JMP NT0: T0 = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.T0, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x61: // JMP Z,Imm
                           if (ScuDsp->ProgControlPort.part.Z)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }
                           break;
                        case 0x62: // JMP S, Imm
                           if (ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }

                           //LOG("scu\t: JMP S: S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x63: // JMP ZS, Imm
                           if (ScuDsp->ProgControlPort.part.Z || ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }

                           //LOG("scu\t: JMP ZS: Z = %d, S = %d, jmpaddr = %08X\n", ScuDsp->ProgControlPort.part.Z, (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x64: // JMP C, Imm
                           if (ScuDsp->ProgControlPort.part.C)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }
                           break;
                        case 0x68: // JMP T0,Imm
                           if (ScuDsp->ProgControlPort.part.T0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0; 
                           }
                           break;
                        default:
                           LOG("scu\t: Unknown JMP instruction not implemented\n");
                           break;
                     }
                     break;
                  case 0x0E: // Loop bottom Commands
                     if (instruction & 0x8000000)
                     {
                        // LPS
                        if (ScuDsp->LOP != 0)
                        {
                           ScuDsp->jmpaddr = ScuDsp->PC;
                           ScuDsp->delayed = 0;
                           ScuDsp->LOP--;
                        }
                     }
                     else
                     {
                        // BTM
                        if (ScuDsp->LOP != 0)
                        {
                           ScuDsp->jmpaddr = ScuDsp->TOP;
                           ScuDsp->delayed = 0;
                           ScuDsp->LOP--;
                        }
                     }

                     break;
                  case 0x0F: // End Commands
                     /* Finish an open DSP DMA at END, otherwise the last DMA of a program
                        stays pending (T0=1) until the next program starts. Same as
                        Kronos ("Fix DOA and crazy Ivan"). */
                     {
                        if (ScuDsp->dsp_dma_wait > 0) {
                           ScuDsp->dsp_dma_wait = 0;
                           step_dsp_dma(ScuDsp);
                        }
                     }
                     ScuDsp->ProgControlPort.part.EX = 0;

                     if (instruction & 0x8000000) {
                        // End with Interrupt
                        ScuDsp->ProgControlPort.part.E = 1;
                        ScuSendDSPEnd();
                     }

                     LOG("dsp has ended\n");
                     ScuDsp->ProgControlPort.part.P = ScuDsp->PC+1;
                     dsp_counter = 1;
                     break;
                  default: break;
               }
               break;
            }
            default: 
               LOG("scu\t: Invalid DSP opcode %08X at offset %02X\n", instruction, ScuDsp->PC);
               break;
         }

         //ScuDsp->MUL.all = (s64)ScuDsp->RX * (s32)ScuDsp->RY;
         
         if (inc & 1u){ ScuDsp->CT[0]++; ScuDsp->CT[0] &= 0x3f; inc &= ~1u; };
         if (inc & 2u){ ScuDsp->CT[1]++; ScuDsp->CT[1] &= 0x3f; inc &= ~2u; };
         if (inc & 4u){ ScuDsp->CT[2]++; ScuDsp->CT[2] &= 0x3f; inc &= ~4u; };
         if (inc & 8u){ ScuDsp->CT[3]++; ScuDsp->CT[3] &= 0x3f; inc &= ~8u; };

         ScuDsp->PC++;

         // Handle delayed jumps
         if (ScuDsp->jmpaddr != 0xFFFFFFFF)
         {
            if (ScuDsp->delayed)
            {
               ScuDsp->PC = (unsigned char)ScuDsp->jmpaddr;
               ScuDsp->jmpaddr = 0xFFFFFFFF;
               dsp_counter += 1; // hold clock
            }
            else
               ScuDsp->delayed = 1;
         }
         dsp_counter--;
      }
   (void)n_bef;
   (void)pcp; (void)alu;
}
