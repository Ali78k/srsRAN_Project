/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "rlc_tx_tm_entity.h"
#include "srsran/support/srsran_assert.h"

using namespace srsran;

rlc_tx_tm_entity::rlc_tx_tm_entity(uint32_t                             du_index,
                                   du_ue_index_t                        ue_index,
                                   rb_id_t                              rb_id,
                                   rlc_tx_upper_layer_data_notifier&    upper_dn_,
                                   rlc_tx_upper_layer_control_notifier& upper_cn_,
                                   rlc_tx_lower_layer_notifier&         lower_dn_,
                                   task_executor&                       pcell_executor_,
                                   bool                                 metrics_enabled_,
                                   rlc_pcap&                            pcap_) :
  rlc_tx_entity(du_index, ue_index, rb_id, upper_dn_, upper_cn_, lower_dn_, metrics_enabled_, pcap_),
  pcell_executor(pcell_executor_),
  pcap_context(ue_index, rb_id, /* is_uplink */ false)
{
  metrics.metrics_set_mode(rlc_mode::tm);
  logger.log_info("RLC TM created.");
}

// TS 38.322 v16.2.0 Sec. 5.2.1.1
void rlc_tx_tm_entity::handle_sdu(rlc_sdu sdu)
{
  size_t sdu_length = sdu.buf.length();
  if (sdu_queue.write(sdu)) {
    logger.log_info(sdu.buf.begin(), sdu.buf.end(), "TX SDU. sdu_len={} {}", sdu.buf.length(), sdu_queue);
    metrics.metrics_add_sdus(1, sdu_length);
    handle_changed_buffer_state();
  } else {
    logger.log_info("Dropped SDU. sdu_len={} {}", sdu.buf.length(), sdu_queue);
    metrics.metrics_add_lost_sdus(1);
  }
}

// TS 38.322 v16.2.0 Sec. 5.4
void rlc_tx_tm_entity::discard_sdu(uint32_t pdcp_sn)
{
  logger.log_warning("Ignoring invalid attempt to discard SDU in TM. pdcp_sn={}", pdcp_sn);
  metrics.metrics_add_discard_failure(1);
}

// TS 38.322 v16.2.0 Sec. 5.2.1.1
size_t rlc_tx_tm_entity::pull_pdu(span<uint8_t> rlc_pdu_buf)
{
  if (sdu_queue.is_empty()) {
    logger.log_debug("SDU queue empty. grant_len={}", rlc_pdu_buf.size());
    return {};
  }

  uint32_t front_len = {};
  if (not sdu_queue.front_size_bytes(front_len)) {
    logger.log_warning("Could not get sdu_len of SDU queue front. grant_len={}", rlc_pdu_buf.size());
    return {};
  }

  if (front_len > rlc_pdu_buf.size()) {
    logger.log_info("SDU/PDU exeeds provided space. front_len={} grant_len={}", front_len, rlc_pdu_buf.size());
    metrics.metrics_add_small_alloc(1);
    return {};
  }

  rlc_sdu sdu = {};
  logger.log_debug("Reading SDU from sdu_queue. {}", sdu_queue);
  if (not sdu_queue.read(sdu)) {
    logger.log_warning("Could not read SDU from non-empty queue. grant_len={} {}", rlc_pdu_buf.size(), sdu_queue);
    return {};
  }

  size_t sdu_len = sdu.buf.length();
  srsran_sanity_check(sdu_len == front_len, "Length mismatch. sdu_len={} front_len={}", sdu_len, front_len);

  // In TM there is no header, just pass the plain SDU
  auto out_it = rlc_pdu_buf.begin();
  for (span<uint8_t> seg : sdu.buf.segments()) {
    out_it = std::copy(seg.begin(), seg.end(), out_it);
  }

  logger.log_info(sdu.buf.begin(), sdu.buf.end(), "TX PDU. pdu_len={} grant_len={}", sdu_len, rlc_pdu_buf.size());

  // Update metrics
  metrics.metrics_add_pdus_no_segmentation(1, sdu_len);

  // Push PDU into PCAP.
  pcap.push_pdu(pcap_context, sdu.buf);

  // Return number of bytes written.
  return out_it - rlc_pdu_buf.begin();
}

void rlc_tx_tm_entity::handle_changed_buffer_state()
{
  if (not pending_buffer_state.test_and_set(std::memory_order_seq_cst)) {
    logger.log_debug("Triggering buffer state update to lower layer");
    // Redirect handling of status to pcell_executor
    if (not pcell_executor.defer([this]() { update_mac_buffer_state(); })) {
      logger.log_error("Failed to enqueue buffer state update");
    }
  } else {
    logger.log_debug("Avoiding redundant buffer state update to lower layer");
  }
}

void rlc_tx_tm_entity::update_mac_buffer_state()
{
  pending_buffer_state.clear(std::memory_order_seq_cst);
  unsigned bs = get_buffer_state();
  logger.log_debug("Sending buffer state update to lower layer. bs={}", bs);
  lower_dn.on_buffer_state_update(bs);
}

uint32_t rlc_tx_tm_entity::get_buffer_state()
{
  return sdu_queue.size_bytes();
}
