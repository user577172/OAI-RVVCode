/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <stdio.h>
#include <stdint.h>
#include "PHY/sse_intrin.h"
#include "../../nrLDPCdecoder_defs.h"
#include "../../nrLDPC_types.h"

static void emit_bn_pc_rvv(FILE *fd, uint32_t degree, uint32_t count_factor,
                           uint32_t data_base, uint32_t llr_base, uint32_t edge_stride)
{
  fprintf(fd, "  {\n    const size_t count = (((size_t) %u * Z + 15) >> 4) * 16;\n", count_factor);
  fprintf(fd, "    for (size_t pos = 0; pos < count;) {\n");
  fprintf(fd, "      const size_t vl = __riscv_vsetvl_e8m2(count - pos);\n");
  if (degree == 1) {
    fprintf(fd, "      const vint8m2_t channel = __riscv_vle8_v_i8m2(llrProcBuf + %u + pos, vl);\n", llr_base);
    fprintf(fd, "      __riscv_vse8_v_i8m2(bnProcBufRes + %u + pos, channel, vl);\n", data_base);
    fprintf(fd, "      const vint8m2_t check = __riscv_vle8_v_i8m2(bnProcBuf + %u + pos, vl);\n", data_base);
    fprintf(fd, "      __riscv_vse8_v_i8m2(llrRes + %u + pos, __riscv_vsadd_vv_i8m2(check, channel, vl), vl);\n", llr_base);
  } else {
    fprintf(fd, "      vint8m2_t x = __riscv_vle8_v_i8m2(bnProcBuf + %u + pos, vl);\n", data_base);
    fprintf(fd, "      vint16m4_t sum = __riscv_vwcvt_x_x_v_i16m4(x, vl);\n");
    for (uint32_t edge = 1; edge < degree; ++edge) {
      fprintf(fd, "      x = __riscv_vle8_v_i8m2(bnProcBuf + %u + pos, vl);\n", data_base + edge * edge_stride);
      fprintf(fd, "      sum = __riscv_vadd_vv_i16m4(sum, __riscv_vwcvt_x_x_v_i16m4(x, vl), vl);\n");
    }
    fprintf(fd, "      x = __riscv_vle8_v_i8m2(llrProcBuf + %u + pos, vl);\n", llr_base);
    fprintf(fd, "      sum = __riscv_vadd_vv_i16m4(sum, __riscv_vwcvt_x_x_v_i16m4(x, vl), vl);\n");
    fprintf(fd, "      sum = __riscv_vmax_vx_i16m4(sum, -128, vl);\n");
    fprintf(fd, "      sum = __riscv_vmin_vx_i16m4(sum, 127, vl);\n");
    fprintf(fd, "      __riscv_vse8_v_i8m2(llrRes + %u + pos, __riscv_vncvt_x_x_w_i8m2(sum, vl), vl);\n", llr_base);
  }
  fprintf(fd, "      pos += vl;\n    }\n  }\n");
}


void nrLDPC_bnProcPc_BG2_generator_128(const char *dir, int R)
{
  const char *ratestr[3]={"15","13","23"};

  if (R<0 || R>2) {printf("Illegal R %d\n",R); abort();}


 // system("mkdir -p ../ldpc_gen_files");

  char fname[FILENAME_MAX+1];
  snprintf(fname, sizeof(fname), "%s/bnProcPc128/nrLDPC_bnProcPc_BG2_R%s_128.h", dir, ratestr[R]);
  FILE *fd=fopen(fname,"w");
  if (fd == NULL) {
    printf("Cannot create file %s\n", fname);
    abort();
  }

  fprintf(fd,"#include <stdint.h>\n");
  fprintf(fd,"#include \"PHY/sse_intrin.h\"\n");
  fprintf(fd,"#if defined(__riscv_vector)\n#include <riscv_vector.h>\n#endif\n");

  fprintf(fd,"static inline void nrLDPC_bnProcPc_BG2_R%s_128(int8_t* bnProcBuf,int8_t* bnProcBufRes,int8_t* llrRes ,  int8_t* llrProcBuf, uint16_t Z  ) {\n",ratestr[R]);
  fprintf(fd,"#if !defined(__riscv_vector)\n");
    const uint8_t*  lut_numBnInBnGroups;
    const uint32_t* lut_startAddrBnGroups;
    const uint16_t* lut_startAddrBnGroupsLlr;
    if (R==0) {


      lut_numBnInBnGroups =  lut_numBnInBnGroups_BG2_R15;
      lut_startAddrBnGroups = lut_startAddrBnGroups_BG2_R15;
      lut_startAddrBnGroupsLlr = lut_startAddrBnGroupsLlr_BG2_R15;

    }
    else if (R==1){

      lut_numBnInBnGroups =  lut_numBnInBnGroups_BG2_R13;
      lut_startAddrBnGroups = lut_startAddrBnGroups_BG2_R13;
      lut_startAddrBnGroupsLlr = lut_startAddrBnGroupsLlr_BG2_R13;
    }
    else if (R==2) {

      lut_numBnInBnGroups = lut_numBnInBnGroups_BG2_R23;
      lut_startAddrBnGroups = lut_startAddrBnGroups_BG2_R23;
      lut_startAddrBnGroupsLlr = lut_startAddrBnGroupsLlr_BG2_R23;
    }
  else { printf("aborting, illegal R %d\n",R); fclose(fd);abort();}

        // Number of BNs in Groups
    uint32_t k;
    // Offset to each bit within a group in terms of 32 Byte
    uint32_t cnOffsetInGroup;
    uint8_t idxBnGroup = 0;

    fprintf(fd,"  // Process group with 1 CN\n");
    fprintf(fd,"        uint32_t M = (%d*Z + 15)>>4;\n",lut_numBnInBnGroups[0]);

    fprintf(fd,"        simde__m128i* p_bnProcBuf    = (simde__m128i*) &bnProcBuf    [%u];\n", lut_startAddrBnGroups[idxBnGroup]);
    fprintf(fd,"        simde__m128i* p_bnProcBufRes = (simde__m128i*) &bnProcBufRes [%u];\n", lut_startAddrBnGroups[idxBnGroup]);
    fprintf(fd,"        simde__m128i* p_llrProcBuf   = (simde__m128i*) &llrProcBuf   [%d];\n", lut_startAddrBnGroupsLlr[idxBnGroup]);
    fprintf(fd,"        simde__m128i* p_llrRes       = (simde__m128i*) &llrRes       [%d];\n", lut_startAddrBnGroupsLlr[idxBnGroup]);
    fprintf(fd,"        simde__m128i ymm0, ymm1, ymmRes0, ymmRes1;\n");


    fprintf(fd,"        for (int i=0;i<M;i++) {\n");
    fprintf(fd,"          p_bnProcBufRes[i] = p_llrProcBuf[i];\n");
    fprintf(fd,"          ymm0 = simde_mm_cvtepi8_epi16(p_bnProcBuf [i]);\n");
    fprintf(fd,"          ymm1 = simde_mm_cvtepi8_epi16(p_llrProcBuf[i]);\n");
    fprintf(fd,"          ymmRes0 = simde_mm_adds_epi16(ymm0, ymm1);\n");
    fprintf(fd,"          ymm0 = simde_mm_cvtepi8_epi16(simde_mm_srli_si128(p_bnProcBuf [i],8));\n");
    fprintf(fd,"          ymm1 = simde_mm_cvtepi8_epi16(simde_mm_srli_si128(p_llrProcBuf[i],8));\n");
    fprintf(fd,"          ymmRes1 = simde_mm_adds_epi16(ymm0, ymm1);\n");
    fprintf(fd,"          *p_llrRes = simde_mm_packs_epi16(ymmRes0, ymmRes1);\n");
    fprintf(fd,"          p_llrRes++;\n");
    fprintf(fd,"        }\n");
 
    
    for (uint32_t cnidx=1;cnidx<30;cnidx++) {
    // Process group with 4 CNs

       if (lut_numBnInBnGroups[cnidx] > 0)
       {
        // If elements in group move to next address
        idxBnGroup++;

        fprintf(fd,"  M = (%d*Z + 15)>>4;\n",lut_numBnInBnGroups[cnidx]);

        // Set the offset to each CN within a group in terms of 16 Byte
        cnOffsetInGroup = (lut_numBnInBnGroups[cnidx]*NR_LDPC_ZMAX)>>4;

        // Set pointers to start of group 2
        fprintf(fd,"  p_bnProcBuf     = (simde__m128i*) &bnProcBuf    [%u];\n", lut_startAddrBnGroups[idxBnGroup]);
        fprintf(fd,"  p_llrProcBuf    = (simde__m128i*) &llrProcBuf   [%d];\n", lut_startAddrBnGroupsLlr[idxBnGroup]);
        fprintf(fd,"  p_llrRes        = (simde__m128i*) &llrRes       [%d];\n", lut_startAddrBnGroupsLlr[idxBnGroup]);

        // Loop over BNs
        fprintf(fd,"        for (int i=0;i<M;i++) {\n");
            // First 16 LLRs of first CN
        fprintf(fd,"        ymmRes0 = simde_mm_cvtepi8_epi16(p_bnProcBuf [i]);\n");
        fprintf(fd,"        ymmRes1 = simde_mm_cvtepi8_epi16(simde_mm_srli_si128(p_bnProcBuf [i],8));\n");

            // Loop over CNs
        for (k=1; k<=cnidx; k++)
        {
           fprintf(fd,"        ymm0 = simde_mm_cvtepi8_epi16(p_bnProcBuf[%u + i]);\n", k * cnOffsetInGroup);
           fprintf(fd,"        ymmRes0 = simde_mm_adds_epi16(ymmRes0, ymm0);\n");

           fprintf(fd,"        ymm1 = simde_mm_cvtepi8_epi16(simde_mm_srli_si128(p_bnProcBuf[%u + i],8));\n", k * cnOffsetInGroup);

           fprintf(fd, "       ymmRes1 = simde_mm_adds_epi16(ymmRes1, ymm1); \n");
        }

            // Add LLR from receiver input
        fprintf(fd,"        ymm0    = simde_mm_cvtepi8_epi16(p_llrProcBuf[i]);\n");
        fprintf(fd,"        ymmRes0 = simde_mm_adds_epi16(ymmRes0, ymm0);\n");

        fprintf(fd,"        ymm1    = simde_mm_cvtepi8_epi16(simde_mm_srli_si128(p_llrProcBuf[i],8));\n");
        fprintf(fd,"        ymmRes1 = simde_mm_adds_epi16(ymmRes1, ymm1);\n");

            // Pack results back to epi8
        fprintf(fd,"        *p_llrRes = simde_mm_packs_epi16(ymmRes0, ymmRes1);\n");
        fprintf(fd,"        p_llrRes++;\n");

        fprintf(fd,"   }\n");
       }

    }


    fprintf(fd,"#else\n");
    emit_bn_pc_rvv(fd, 1, lut_numBnInBnGroups[0], lut_startAddrBnGroups[0], lut_startAddrBnGroupsLlr[0], 0);
    uint8_t rvv_idx = 0;
    for (uint32_t cnidx = 1; cnidx < 30; ++cnidx) {
      if (lut_numBnInBnGroups[cnidx] == 0)
        continue;
      ++rvv_idx;
      const uint32_t stride = ((lut_numBnInBnGroups[cnidx] * NR_LDPC_ZMAX) >> 4) * 16u;
      emit_bn_pc_rvv(fd, cnidx + 1, lut_numBnInBnGroups[cnidx],
                     lut_startAddrBnGroups[rvv_idx], lut_startAddrBnGroupsLlr[rvv_idx], stride);
    }
    fprintf(fd,"#endif\n}\n");
    fclose(fd);
}//end of the function  nrLDPC_bnProcPc_BG2





