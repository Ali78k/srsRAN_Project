/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "ldpc_encoder_neon.h"
#include "neon_support.h"
#include "srsran/srsvec/binary.h"
#include "srsran/srsvec/circ_shift.h"
#include "srsran/srsvec/copy.h"
#include "srsran/srsvec/zero.h"

using namespace srsran;
using namespace srsran::ldpc;

/// Maximum number of NEON vectors needed to represent a BG node.
static constexpr unsigned MAX_NODE_SIZE_NEON = divide_ceil(ldpc::MAX_LIFTING_SIZE, NEON_SIZE_BYTE);

// Recursively selects the proper strategy for the high-rate region by successively decreasing the value of the template
// parameter.
template <unsigned NODE_SIZE_NEON_PH>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_hr_strategy(ldpc_base_graph_type current_bg,
                                                                         uint8_t              current_ls_index,
                                                                         unsigned int         node_size_neon)
{
  if (node_size_neon != NODE_SIZE_NEON_PH) {
    return select_hr_strategy<NODE_SIZE_NEON_PH - 1>(current_bg, current_ls_index, node_size_neon);
  }

  if (current_bg == ldpc_base_graph_type::BG1) {
    if (current_ls_index == 6) {
      return &ldpc_encoder_neon::high_rate_bg1_i6_inner<NODE_SIZE_NEON_PH>;
    }
    // If current_lifting_index is not 6...
    return &ldpc_encoder_neon::high_rate_bg1_other_inner<NODE_SIZE_NEON_PH>;
  }
  // Else, if current_bg == BG2...
  if ((current_ls_index == 3) || (current_ls_index == 7)) {
    return &ldpc_encoder_neon::high_rate_bg2_i3_7_inner<NODE_SIZE_NEON_PH>;
  }
  // If current_lifting_index is neither 3 nor 7...
  return &ldpc_encoder_neon::high_rate_bg2_other_inner<NODE_SIZE_NEON_PH>;
}

// Ensures that the recursion stops when NODE_SIZE_NEON_PH == 1.
template <>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_hr_strategy<1>(ldpc_base_graph_type current_bg,
                                                                            uint8_t              current_ls_index,
                                                                            unsigned /*node_size_neon*/)
{
  if (current_bg == ldpc_base_graph_type::BG1) {
    if (current_ls_index == 6) {
      return &ldpc_encoder_neon::high_rate_bg1_i6_inner<1>;
    }
    // If current_lifting_index is not 6...
    return &ldpc_encoder_neon::high_rate_bg1_other_inner<1>;
  }
  // Else, if current_bg == BG2...
  if ((current_ls_index == 3) || (current_ls_index == 7)) {
    return &ldpc_encoder_neon::high_rate_bg2_i3_7_inner<1>;
  }
  // If current_lifting_index is neither 3 nor 7...
  return &ldpc_encoder_neon::high_rate_bg2_other_inner<1>;
}

static constexpr unsigned BG1_K = BG1_N_FULL - BG1_M;
static constexpr unsigned BG2_K = BG2_N_FULL - BG2_M;

// Recursively selects the proper strategy for the systematic bits by successively decreasing the value of the template
// parameter.
template <unsigned NODE_SIZE_NEON_PH>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_sys_bits_strategy(ldpc_base_graph_type current_bg,
                                                                               unsigned             node_size_neon)
{
  if (node_size_neon != NODE_SIZE_NEON_PH) {
    return select_sys_bits_strategy<NODE_SIZE_NEON_PH - 1>(current_bg, node_size_neon);
  }
  if (current_bg == ldpc_base_graph_type::BG1) {
    return &ldpc_encoder_neon::systematic_bits_inner<BG1_K, BG1_M, NODE_SIZE_NEON_PH>;
  }
  return &ldpc_encoder_neon::systematic_bits_inner<BG2_K, BG2_M, NODE_SIZE_NEON_PH>;
}

// Ensures that the recursion stops when NODE_SIZE_NEON_PH == 1.
template <>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_sys_bits_strategy<1>(ldpc_base_graph_type current_bg,
                                                                                  unsigned /*node_size_neon*/)
{
  if (current_bg == ldpc_base_graph_type::BG1) {
    return &ldpc_encoder_neon::systematic_bits_inner<BG1_K, BG1_M, 1>;
  }
  return &ldpc_encoder_neon::systematic_bits_inner<BG2_K, BG2_M, 1>;
}

// Recursively selects the proper strategy for the extended region by successively decreasing the value of the template
// parameter.
template <unsigned NODE_SIZE_NEON_PH>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_ext_strategy(unsigned node_size_neon)
{
  if (node_size_neon == NODE_SIZE_NEON_PH) {
    return &ldpc_encoder_neon::ext_region_inner<NODE_SIZE_NEON_PH>;
  }
  return select_ext_strategy<NODE_SIZE_NEON_PH - 1>(node_size_neon);
}

// Ensures that the recursion stops when NODE_SIZE_NEON_PH == 1.
template <>
ldpc_encoder_neon::strategy_method ldpc_encoder_neon::select_ext_strategy<1>(unsigned /*node_size_neon*/)
{
  return &ldpc_encoder_neon::ext_region_inner<1>;
}

void ldpc_encoder_neon::select_strategy()
{
  ldpc_base_graph_type current_bg       = current_graph->get_base_graph();
  uint8_t              current_ls_index = current_graph->get_lifting_index();

  // Each BG node contains LS bits, which are stored in node_size_neon NEON vectors.
  node_size_neon = divide_ceil(lifting_size, NEON_SIZE_BYTE);

  systematic_bits = select_sys_bits_strategy<MAX_NODE_SIZE_NEON>(current_bg, node_size_neon);
  high_rate       = select_hr_strategy<MAX_NODE_SIZE_NEON>(current_bg, current_ls_index, node_size_neon);
  ext_region      = select_ext_strategy<MAX_NODE_SIZE_NEON>(node_size_neon);
}

void ldpc_encoder_neon::load_input(span<const uint8_t> in)
{
  unsigned node_size_byte = node_size_neon * NEON_SIZE_BYTE;
  unsigned tail_bytes     = node_size_byte - lifting_size;

  // Set state variables depending on the codeblock length.
  codeblock_used_size = codeblock_length / lifting_size * node_size_neon;
  auxiliary_used_size = (codeblock_length / lifting_size - bg_K) * node_size_neon;

  span<uint8_t>       codeblock(codeblock_buffer.data(), codeblock_used_size * NEON_SIZE_BYTE);
  span<const uint8_t> in_tmp = in;
  for (unsigned i_node = 0; i_node != bg_K; ++i_node) {
    srsvec::copy(codeblock.first(lifting_size), in_tmp.first(lifting_size));
    codeblock = codeblock.last(codeblock.size() - lifting_size);
    in_tmp    = in_tmp.last(in_tmp.size() - lifting_size);

    srsvec::zero(codeblock.first(tail_bytes));
    codeblock = codeblock.last(codeblock.size() - tail_bytes);
  }
  srsvec::zero(codeblock);
}

// Computes the XOR logical operation (modulo-2 sum) between the contents of "in0" and "in1". The result is stored in
// "out". The representation is unpacked (1 byte represents 1 bit).
static void fast_xor(span<int8_t> out, span<const int8_t> in0, span<const int8_t> in1)
{
  unsigned i = 0;

  const int8_t* in0_ptr = reinterpret_cast<const int8_t*>(in0.data());
  const int8_t* in1_ptr = reinterpret_cast<const int8_t*>(in1.data());
  int8_t*       out_ptr = reinterpret_cast<int8_t*>(out.data());

  for (unsigned i_end = (out.size() / 16) * 16; i != i_end; i += 16) {
    int8x16_t in0_ = vld1q_s8(in0_ptr + i);
    int8x16_t in1_ = vld1q_s8(in1_ptr + i);

    vst1q_s8(out_ptr + i, veorq_s8(in0_, in1_));
  }

  for (unsigned i_end = out.size(); i != i_end; ++i) {
    out[i] = in0[i] ^ in1[i];
  }
}

// Computes the AND logical operation between the contents of "in"  (one byte represents one bit) and "1". The r>
// stored in "out". This is done to set filler bits (represented by a large even number) to zero, as understood >
// encoder.
static void fast_and_one(span<int8_t> out, span<const int8_t> in)
{
  unsigned i = 0;

  const int8_t* in0_ptr = reinterpret_cast<const int8_t*>(in.data());
  int8_t*       out_ptr = reinterpret_cast<int8_t*>(out.data());
  for (unsigned i_end = (out.size() / 16) * 16; i != i_end; i += 16) {
    int8x16_t in0_ = vld1q_s8(in0_ptr + i);

    vst1q_s8(out_ptr + i, vandq_s8(in0_, vdupq_n_s8(1)));
  }

  for (unsigned i_end = out.size(); i != i_end; ++i) {
    out[i] = in[i] & 1;
  }
}

template <unsigned BG_K_PH, unsigned BG_M_PH, unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::systematic_bits_inner()
{
  neon::neon_const_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  const auto& parity_check_sparse = current_graph->get_parity_check_sparse();

  std::array<int8_t, 2 * MAX_LIFTING_SIZE> tmp_blk       = {};
  span<int8_t>                             blk           = span<int8_t>(tmp_blk).first(2 * lifting_size);
  unsigned                                 current_i_blk = std::numeric_limits<unsigned>::max();

  std::array<bool, BG_M_PH> m_mask = {};

  for (const auto& element : parity_check_sparse) {
    unsigned m          = std::get<0>(element);
    unsigned k          = std::get<1>(element);
    unsigned node_shift = std::get<2>(element);

    unsigned i_aux = m * NODE_SIZE_NEON_PH;
    unsigned i_blk = k * NODE_SIZE_NEON_PH;

    if (i_aux >= auxiliary_used_size) {
      continue;
    }

    if (i_blk != current_i_blk) {
      current_i_blk = i_blk;
      fast_and_one(blk.first(lifting_size), codeblock.plain_span(current_i_blk, lifting_size));
      fast_and_one(blk.last(lifting_size), codeblock.plain_span(current_i_blk, lifting_size));
    }

    span<int8_t> plain_auxiliary = auxiliary.plain_span(i_aux, lifting_size);

    // If m >= 4, we can write directly to the codeblock buffer.
    if (m >= 4) {
      unsigned offset = (bg_K + m) * NODE_SIZE_NEON_PH * NEON_SIZE_BYTE;
      plain_auxiliary = span<int8_t>(reinterpret_cast<int8_t*>(codeblock_buffer.data()) + offset, lifting_size);
    }

    if (m_mask[m]) {
      fast_xor(plain_auxiliary, blk.subspan(node_shift, lifting_size), plain_auxiliary);
    } else {
      srsvec::copy(plain_auxiliary, blk.subspan(node_shift, lifting_size));
      m_mask[m] = true;
    }
  }
}

template <unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::high_rate_bg1_i6_inner()
{
  unsigned skip0         = bg_K * NODE_SIZE_NEON_PH;
  unsigned skip1         = (bg_K + 1) * NODE_SIZE_NEON_PH;
  unsigned skip2         = (bg_K + 2) * NODE_SIZE_NEON_PH;
  unsigned skip3         = (bg_K + 3) * NODE_SIZE_NEON_PH;
  unsigned node_size_x_2 = 2 * NODE_SIZE_NEON_PH;
  unsigned node_size_x_3 = 3 * NODE_SIZE_NEON_PH;

  neon::neon_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_const_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  neon::neon_span rotated_node(rotated_node_buffer, NODE_SIZE_NEON_PH);

  // First chunk of parity bits.
  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t tmp = veorq_s8(auxiliary.get_at(j), auxiliary.get_at(NODE_SIZE_NEON_PH + j));
    tmp           = veorq_s8(tmp, auxiliary.get_at(node_size_x_2 + j));
    tmp           = veorq_s8(tmp, auxiliary.get_at(node_size_x_3 + j));
    rotated_node.set_at(j, tmp);
  }

  srsvec::circ_shift_forward(
      codeblock.plain_span(skip0, lifting_size), rotated_node.plain_span(0, lifting_size), 105 % lifting_size);

  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t block0 = codeblock.get_at(skip0 + j);
    // Second chunk of parity bits.
    codeblock.set_at(skip1 + j, veorq_s8(auxiliary.get_at(j), block0));
    // Fourth chunk of parity bits.
    int8x16_t block3 = veorq_s8(auxiliary.get_at(node_size_x_3 + j), block0);
    codeblock.set_at(skip3 + j, block3);
    // Third chunk of parity bits.
    codeblock.set_at(skip2 + j, veorq_s8(auxiliary.get_at(node_size_x_2 + j), block3));
  }
}

template <unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::high_rate_bg1_other_inner()
{
  unsigned skip0         = bg_K * NODE_SIZE_NEON_PH;
  unsigned skip1         = (bg_K + 1) * NODE_SIZE_NEON_PH;
  unsigned skip2         = (bg_K + 2) * NODE_SIZE_NEON_PH;
  unsigned skip3         = (bg_K + 3) * NODE_SIZE_NEON_PH;
  unsigned node_size_x_2 = 2 * NODE_SIZE_NEON_PH;
  unsigned node_size_x_3 = 3 * NODE_SIZE_NEON_PH;

  neon::neon_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_const_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  neon::neon_span rotated_node(rotated_node_buffer, NODE_SIZE_NEON_PH);

  // First chunk of parity bits.
  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t block0 = veorq_s8(auxiliary.get_at(j), auxiliary.get_at(NODE_SIZE_NEON_PH + j));
    block0           = veorq_s8(block0, auxiliary.get_at(node_size_x_2 + j));
    block0           = veorq_s8(block0, auxiliary.get_at(node_size_x_3 + j));
    codeblock.set_at(skip0 + j, block0);
  }

  srsvec::circ_shift_backward(rotated_node.plain_span(0, lifting_size), codeblock.plain_span(skip0, lifting_size), 1);

  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t rotated_j = rotated_node.get_at(j);
    // Second chunk of parity bits.
    codeblock.set_at(skip1 + j, veorq_s8(auxiliary.get_at(j), rotated_j));
    // Fourth chunk of parity bits.
    int8x16_t block3 = veorq_s8(auxiliary.get_at(node_size_x_3 + j), rotated_j);
    codeblock.set_at(skip3 + j, block3);
    // Third chunk of parity bits.
    codeblock.set_at(skip2 + j, veorq_s8(auxiliary.get_at(node_size_x_2 + j), block3));
  }
}

template <unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::high_rate_bg2_i3_7_inner()
{
  unsigned skip0         = bg_K * NODE_SIZE_NEON_PH;
  unsigned skip1         = (bg_K + 1) * NODE_SIZE_NEON_PH;
  unsigned skip2         = (bg_K + 2) * NODE_SIZE_NEON_PH;
  unsigned skip3         = (bg_K + 3) * NODE_SIZE_NEON_PH;
  unsigned node_size_x_2 = 2 * NODE_SIZE_NEON_PH;
  unsigned node_size_x_3 = 3 * NODE_SIZE_NEON_PH;

  neon::neon_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_const_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  neon::neon_span rotated_node(rotated_node_buffer, NODE_SIZE_NEON_PH);

  // First chunk of parity bits.
  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t block0 = veorq_s8(auxiliary.get_at(j), auxiliary.get_at(NODE_SIZE_NEON_PH + j));
    block0           = veorq_s8(block0, auxiliary.get_at(node_size_x_2 + j));
    block0           = veorq_s8(block0, auxiliary.get_at(node_size_x_3 + j));
    codeblock.set_at(skip0 + j, block0);
  }

  srsvec::circ_shift_backward(rotated_node.plain_span(0, lifting_size), codeblock.plain_span(skip0, lifting_size), 1);

  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t rotated_j = rotated_node.get_at(j);
    // Second chunk of parity bits.
    int8x16_t block_1 = veorq_s8(auxiliary.get_at(j), rotated_j);
    codeblock.set_at(skip1 + j, block_1);
    // Third chunk of parity bits.
    codeblock.set_at(skip2 + j, veorq_s8(auxiliary.get_at(NODE_SIZE_NEON_PH + j), block_1));
    // Fourth chunk of parity bits.
    codeblock.set_at(skip3 + j, veorq_s8(auxiliary.get_at(node_size_x_3 + j), rotated_j));
  }
}

template <unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::high_rate_bg2_other_inner()
{
  unsigned skip0         = bg_K * NODE_SIZE_NEON_PH;
  unsigned skip1         = (bg_K + 1) * NODE_SIZE_NEON_PH;
  unsigned skip2         = (bg_K + 2) * NODE_SIZE_NEON_PH;
  unsigned skip3         = (bg_K + 3) * NODE_SIZE_NEON_PH;
  unsigned node_size_x_2 = 2 * NODE_SIZE_NEON_PH;
  unsigned node_size_x_3 = 3 * NODE_SIZE_NEON_PH;

  neon::neon_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_const_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  neon::neon_span rotated_node(rotated_node_buffer, NODE_SIZE_NEON_PH);

  // First chunk of parity bits.
  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t rotated_j = veorq_s8(auxiliary.get_at(j), auxiliary.get_at(NODE_SIZE_NEON_PH + j));
    rotated_j           = veorq_s8(rotated_j, auxiliary.get_at(node_size_x_2 + j));
    rotated_j           = veorq_s8(rotated_j, auxiliary.get_at(node_size_x_3 + j));
    rotated_node.set_at(j, rotated_j);
  }

  srsvec::circ_shift_forward(codeblock.plain_span(skip0, lifting_size), rotated_node.plain_span(0, lifting_size), 1);

  for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
    int8x16_t block_0 = codeblock.get_at(skip0 + j);
    // Second chunk of parity bits.
    int8x16_t block_1 = veorq_s8(auxiliary.get_at(j), block_0);
    codeblock.set_at(skip1 + j, block_1);
    // Third chunk of parity bits.
    codeblock.set_at(skip2 + j, veorq_s8(auxiliary.get_at(NODE_SIZE_NEON_PH + j), block_1));
    // Fourth chunk of parity bits.
    codeblock.set_at(skip3 + j, veorq_s8(auxiliary.get_at(node_size_x_3 + j), block_0));
  }
}

template <unsigned NODE_SIZE_NEON_PH>
void ldpc_encoder_neon::ext_region_inner()
{
  // We only compute the variable nodes needed to fill the codeword.
  // Also, recall the high-rate region has length (bg_K + 4) * lifting_size.
  unsigned nof_layers = codeblock_length / lifting_size - bg_K;

  neon::neon_span codeblock(codeblock_buffer, codeblock_used_size);

  neon::neon_const_span auxiliary(auxiliary_buffer, auxiliary_used_size);

  neon::neon_span rotated_node(rotated_node_buffer, NODE_SIZE_NEON_PH);

  // Encode the extended region.
  for (unsigned m = 4; m != nof_layers; ++m) {
    unsigned skip = (bg_K + m) * NODE_SIZE_NEON_PH;

    for (unsigned k = 0; k != 4; ++k) {
      unsigned node_shift = current_graph->get_lifted_node(m, bg_K + k);

      if (node_shift == NO_EDGE) {
        continue;
      }

      // At this point, the codeblock nodes in the extended region already contain the contribution from the systematic
      // bits (they were computed in systematic_bits_inner). All is left to do is to sum the contribution due to the
      // high-rate region (also stored in codeblock), with the proper circular shifts.
      srsvec::circ_shift_backward(rotated_node.plain_span(0, lifting_size),
                                  codeblock.plain_span((bg_K + k) * NODE_SIZE_NEON_PH, lifting_size),
                                  node_shift);

      for (unsigned j = 0; j != NODE_SIZE_NEON_PH; ++j) {
        codeblock.set_at(skip + j, veorq_s8(codeblock.get_at(skip + j), rotated_node.get_at(j)));
      }
    }
    skip += NODE_SIZE_NEON_PH;
  }
}

void ldpc_encoder_neon::write_codeblock(span<uint8_t> out)
{
  unsigned nof_nodes = codeblock_length / lifting_size;

  // The first two blocks are shortened and the last node is not considered, since it can be incomplete.
  unsigned            node_size_byte = node_size_neon * NEON_SIZE_BYTE;
  span<uint8_t>       out_tmp        = out;
  span<const uint8_t> codeblock(codeblock_buffer);
  codeblock = codeblock.last(codeblock.size() - 2 * node_size_byte);
  for (unsigned i_node = 2, max_i_node = nof_nodes - 1; i_node != max_i_node; ++i_node) {
    srsvec::copy(out_tmp.first(lifting_size), codeblock.first(lifting_size));
    out_tmp   = out_tmp.last(out_tmp.size() - lifting_size);
    codeblock = codeblock.last(codeblock.size() - node_size_byte);
  }

  // Take care of the last node.
  unsigned remainder = out_tmp.size();
  srsvec::copy(out_tmp, codeblock.first(remainder));
}
