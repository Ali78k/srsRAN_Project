/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "../policy/scheduler_policy.h"
#include "../support/slot_event_list.h"
#include "../support/slot_sync_point.h"
#include "srsgnb/adt/slot_array.h"
#include "srsgnb/adt/unique_function.h"
#include "ue_cell_grid_allocator.h"
#include "ue_event_manager.h"
#include "ue_scheduler.h"
#include "ue_srb0_scheduler.h"

namespace srsgnb {

/// \brief Interface of data scheduler that is used to allocate UE DL and UL grants in a given slot.
/// The data_scheduler object will be common to all cells and slots.
class ue_scheduler_impl final : public ue_scheduler
{
public:
  explicit ue_scheduler_impl(sched_configuration_notifier& mac_notif);

  void add_cell(const ue_scheduler_cell_params& params) override;

  /// Schedule UE DL grants for a given {slot, cell}.
  void run_slot(slot_point slot_tx, du_cell_index_t cell_index) override;

  scheduler_ue_configurator& get_ue_configurator() override { return event_mng; }

  scheduler_feedback_handler& get_feedback_handler() override { return event_mng; }

  scheduler_dl_buffer_state_indication_handler& get_dl_buffer_state_indication_handler() override { return event_mng; }

private:
  void run_sched_strategy(slot_point sl_tx);

  struct cell {
    cell_resource_allocator* cell_res_alloc;

    /// SRB0 scheduler
    ue_srb0_scheduler srb0_sched;

    cell(const ue_scheduler_cell_params& params, ue_list& ues) :
      cell_res_alloc(params.cell_res_alloc),
      srb0_sched(params.cell_res_alloc->cfg, *params.pdcch_sched, *params.pucch_sched, ues)
    {
    }
  };

  std::array<std::unique_ptr<cell>, MAX_NOF_DU_CELLS> cells;

  /// Scheduling Strategy
  std::unique_ptr<scheduler_policy> sched_strategy;

  /// Repository of created UEs.
  ue_list ue_db;

  /// Allocator of grants in the resource grid.
  ue_cell_grid_allocator ue_alloc;

  /// Processor of UE input events.
  ue_event_manager event_mng;

  /// Mutex used to lock carriers for joint carrier scheduling.
  slot_sync_point sync_point;
};

} // namespace srsgnb
