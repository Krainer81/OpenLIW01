#pragma once

#include "esphome/core/component.h"

#include <Arduino.h>
#include <SPI.h>

#include <cstdint>
#include <string>

namespace esphome {
namespace liw01_counter_v030 {

class LIW01Counter : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  uint8_t get_raw() const { return this->raw_arduino_; }
  uint8_t get_raw_direct() const { return this->raw_direct_; }
  uint8_t get_wire_arduino() const { return this->wire_arduino_; }
  uint8_t get_wire_direct() const { return this->wire_direct_; }
  bool get_spi_ab_equal() const { return this->raw_arduino_ == this->raw_direct_; }
  bool get_pl_pad_ok() const { return this->pl_pad_ok_; }
  bool get_ce_pad_ok() const { return this->ce_pad_ok_; }
  bool get_gpio_mux_ok() const { return this->gpio_mux_ok_; }
  uint32_t get_gpio_pad_fault_count() const { return this->gpio_pad_fault_count_; }

  // Legacy verified HC590 /CCLR diagnostic. Not exposed in production YAML; the function itself never writes FRAM.
  void manual_hc590_clear_test();
  uint32_t get_hc590_clear_test_count() const { return this->hc590_clear_test_count_; }
  bool get_hc590_clear_test_last_ok() const { return this->hc590_clear_test_last_ok_; }
  bool get_hc590_clear_pulse_verified() const { return this->hc590_clear_pulse_verified_; }
  bool get_hc590_clear_restore_ok() const { return this->hc590_clear_restore_ok_; }

  uint32_t get_overflow_count() const { return this->overflow_count_; }
  uint32_t get_hw32() const { return this->hw32_; }
  uint32_t get_raw255_count() const { return this->raw255_count_; }

  bool get_overflow_pending() const;
  bool get_hc590_cclr_high() const;
  bool get_fram_signature_ok() const { return this->fram_signature_ok_; }
  bool get_master_valid() const { return this->master_valid_; }
  bool get_copy_valid() const { return this->copy_valid_; }

  std::string get_hw32_hex() const;
  std::string get_spi_registers_text() const;
  std::string get_gpio_pad_text() const;
  std::string get_gpio_registers_text() const;
  std::string get_hc590_clear_test_text() const;
  std::string get_fram_signature_text() const { return this->fram_signature_text_; }
  std::string get_master_total_text() const;
  std::string get_copy_total_text() const;
  std::string get_state_text() const;

  // Full external FRAM snapshot remains strictly READ ONLY.
  void dump_full_fram_readonly();
  uint32_t get_fram_dump_count() const { return this->fram_dump_count_; }
  bool get_fram_dump_last_equal() const { return this->fram_dump_last_equal_; }
  std::string get_fram_dump_status_text() const;

  // V0.2.3 production counter / FRAM V2 API.
  uint64_t get_total_liters() const { return this->total_liters_; }
  double get_total_m3() const { return static_cast<double>(this->total_liters_) / 1000.0; }
  uint64_t get_since_v020_liters() const {
    return this->total_liters_ >= this->install_baseline_liters_
               ? this->total_liters_ - this->install_baseline_liters_
               : 0;
  }
  uint64_t get_install_baseline_liters() const { return this->install_baseline_liters_; }
  uint32_t get_counter_event_count() const { return this->counter_event_count_; }
  uint32_t get_counter_last_delta() const { return this->counter_last_delta_; }
  uint32_t get_counter_rejected_delta_count() const { return this->counter_rejected_delta_count_; }
  uint32_t get_counter_ab_retry_count() const { return this->counter_ab_retry_count_; }
  uint32_t get_counter_ab_unresolved_count() const { return this->counter_ab_unresolved_count_; }
  uint32_t get_raw_rollover_count() const { return this->raw_rollover_count_; }
  bool get_counter_synced() const { return this->counter_synced_; }

  bool get_v2_storage_ready() const { return this->v2_storage_ready_; }
  bool get_v2_slot_a_valid() const { return this->v2_slot_a_valid_; }
  bool get_v2_slot_b_valid() const { return this->v2_slot_b_valid_; }
  uint32_t get_v2_sequence() const { return this->v2_sequence_; }
  uint32_t get_v2_write_count() const { return this->v2_write_count_; }
  uint32_t get_v2_write_fault_count() const { return this->v2_write_fault_count_; }
  uint64_t get_last_persisted_total() const { return this->last_persisted_total_; }
  uint64_t get_unsaved_delta() const {
    return this->total_liters_ >= this->last_persisted_total_
               ? this->total_liters_ - this->last_persisted_total_
               : 0;
  }
  bool get_last_persist_ok() const { return this->last_persist_ok_; }
  std::string get_v2_storage_state_text() const;
  std::string get_production_state_text() const;

  // V0.2.3 automatic startup-clear recovery. A software reboot on the real LIW-01 was
  // observed to leave the HC590 output register stale while its internal counter restarted
  // at 0. A first-movement stale signature is recovered automatically. If a large mismatch
  // is already present in the first boot snapshot, it is conservatively adopted with +0 L
  // and recorded diagnostically; no operator confirmation button is required.
  uint32_t get_startup_recovery_count() const { return this->startup_recovery_count_; }
  uint32_t get_startup_ambiguous_adopt_count() const { return this->startup_ambiguous_adopt_count_; }
  std::string get_startup_recovery_state_text() const;

  // Persistent one-time migration marker for the exact V0.2.1 missed +2 L field history.
  // V0.2.3 evaluates/applies this automatically; there is no user-facing repair button.
  bool get_v021_field_repair_applied() const { return this->v021_field_repair_applied_; }
  bool get_v023_migration_evaluated() const { return this->v023_migration_evaluated_; }

  // V0.3.0 isolated service/configuration layer. These operations deliberately do NOT
  // clear HC590, do NOT touch original FRAM 0x000..0x035, and do NOT alter the normal
  // V0.2.3 counting algorithm. They establish a fresh accepted raw baseline and commit
  // the requested software epoch redundantly to the existing V2 A/B format.
  bool service_set_total_liters(uint64_t requested_total_l);
  bool service_reset_counter();
  uint32_t get_service_set_count() const { return this->service_set_count_; }
  uint32_t get_service_reset_count() const { return this->service_reset_count_; }
  uint32_t get_service_refused_count() const { return this->service_refused_count_; }
  bool get_service_last_ok() const { return this->service_last_ok_; }
  uint64_t get_service_last_requested_total() const { return this->service_last_requested_total_; }
  std::string get_service_state_text() const { return this->service_state_text_; }

  // V0.1.8 24-hour validation epoch. START intentionally clears only the volatile
  // HC590/C755 hardware epoch; it never writes original FRAM.
  void start_validation_24h();
  void stop_validation_24h();
  uint64_t get_validation_liters() const { return this->validation_liters_; }
  uint32_t get_validation_elapsed_s() const;
  uint32_t get_validation_last_delta() const { return this->validation_last_delta_; }
  uint32_t get_validation_event_count() const { return this->validation_event_count_; }
  uint32_t get_validation_jump_count() const { return this->validation_jump_count_; }
  uint32_t get_validation_max_delta() const { return this->validation_max_delta_; }
  uint32_t get_validation_rollover_count() const { return this->validation_rollover_count_; }
  uint32_t get_validation_ab_retry_count() const { return this->validation_ab_retry_count_; }
  uint32_t get_validation_ab_unresolved_count() const { return this->validation_ab_unresolved_count_; }
  uint32_t get_validation_anomaly_count() const { return this->validation_anomaly_count_; }
  uint32_t get_validation_raw255_count() const { return this->validation_raw255_count_; }
  uint32_t get_validation_gpio_fault_count() const;
  bool get_validation_running() const { return this->validation_running_; }
  bool get_validation_complete() const { return this->validation_complete_; }
  std::string get_validation_state_text() const;
  std::string get_validation_summary_text() const;

 protected:
  static constexpr uint8_t PIN_HC165_CE = 0;      // GPIO0 /CE, active LOW
  static constexpr uint8_t PIN_HC165_PL = 1;      // GPIO1 /PL, active LOW
  static constexpr uint8_t PIN_HC590_CCLR = 2;    // GPIO2 /CCLR
  static constexpr uint8_t PIN_C755_CLR = 3;      // GPIO3 /CLR, active LOW
  static constexpr uint8_t PIN_C755_Q = 4;        // GPIO4 Q, overflow pending
  static constexpr uint8_t PIN_FRAM_CS = 15;      // GPIO15 /CS, active LOW

  static constexpr uint32_t SPI_HZ = 50000;
  static constexpr uint32_t ORIGINAL_SPI_CLOCK = 0x013D3280UL;
  static constexpr uint32_t FRAM_REFRESH_MS = 300000UL;       // 5 min
  static constexpr uint32_t SPI_REG_LOG_MS = 300000UL;        // 5 min
  static constexpr uint32_t VALIDATION_HEARTBEAT_MS = 60000UL;
  static constexpr uint32_t VALIDATION_DURATION_MS = 86400000UL;  // 24 h

  // V0.2 FRAM V2 layout. Original ZAMEL/SUPLA storage is 0x000..0x035 and is never written.
  static constexpr uint16_t V2_SLOT_A_ADDR = 0x0040;
  static constexpr uint16_t V2_SLOT_B_ADDR = 0x0080;
  static constexpr size_t V2_RECORD_SIZE = 48;
  static constexpr uint32_t V2_MAGIC = 0x3257494CUL;       // bytes: "LIW2"
  static constexpr uint16_t V2_VERSION = 2;
  static constexpr uint32_t V2_COMMIT = 0x5AA5C33CUL;
  static constexpr uint64_t V2_INITIAL_TOTAL_L = 221000ULL;  // approximate physical meter baseline
  static constexpr uint8_t V2_FLAG_LAST_RAW_VALID = 0x01;
  static constexpr uint8_t V2_FLAG_INITIAL_APPROX = 0x02;
  static constexpr uint8_t V2_FLAG_V021_FIELD_REPAIR_APPLIED = 0x04;
  static constexpr uint8_t V2_FLAG_V023_MIGRATION_EVALUATED = 0x08;
  static constexpr uint8_t MAX_DELTA_PER_SAMPLE = 64;
  static constexpr uint8_t STARTUP_CLEAR_RECOVERY_MAX_RAW = 64;
  static constexpr uint32_t PERSIST_RETRY_MS = 5000UL;
  static constexpr uint32_t REJECT_LOG_RATE_MS = 5000UL;

  uint8_t raw_arduino_{0};
  uint8_t raw_direct_{0};
  uint8_t wire_arduino_{0};
  uint8_t wire_direct_{0};
  uint32_t overflow_count_{0};
  uint32_t hw32_{0};
  uint32_t raw255_count_{0};

  // GPIO0 /CE and GPIO1 /PL pad-level diagnostics.
  uint8_t pl_before_{0};
  uint8_t pl_low_imm_{0};
  uint8_t pl_low_end_{0};
  uint8_t pl_high_imm_{0};
  uint8_t pl_high_end_{0};
  uint8_t ce_before_{0};
  uint8_t ce_low_imm_{0};
  uint8_t ce_during_{0};
  uint8_t ce_high_imm_{0};
  bool pl_pad_ok_{false};
  bool ce_pad_ok_{false};
  bool gpio_mux_ok_{false};
  uint32_t gpio_pad_fault_count_{0};

  uint32_t hc590_clear_test_count_{0};
  bool hc590_clear_test_last_ok_{false};
  bool hc590_clear_pulse_verified_{false};
  bool hc590_clear_restore_ok_{false};
  bool hc590_clear_test_ran_{false};
  uint8_t hc590_clear_raw_before_a_{0};
  uint8_t hc590_clear_raw_before_b_{0};
  uint8_t hc590_clear_raw_after_a_{0};
  uint8_t hc590_clear_raw_after_b_{0};
  uint8_t hc590_clear_gpio2_before_{0};
  uint8_t hc590_clear_gpio2_after_{0};
  uint8_t hc590_clear_oe2_before_{0};
  uint8_t hc590_clear_pad_low1_{1};
  uint8_t hc590_clear_gpi_low1_{1};
  uint8_t hc590_clear_pad_low2_{1};
  uint8_t hc590_clear_gpi_low2_{1};
  uint8_t hc590_clear_oe2_during_{0};
  uint32_t hc590_clear_gpf2_before_{0};
  uint32_t hc590_clear_gpf2_during_{0};
  uint32_t hc590_clear_timestamp_ms_{0};

  uint32_t gpio_gpo_{0};
  uint32_t gpio_gpe_{0};
  uint32_t gpio_gpi_{0};
  uint32_t gpio_gpf0_{0};
  uint32_t gpio_gpf1_{0};
  uint32_t gpio_gpc0_{0};
  uint32_t gpio_gpc1_{0};

  uint32_t arduino_spi_clk_{0};
  uint32_t arduino_spi_u_{0};
  uint32_t arduino_spi_u1_{0};
  uint32_t arduino_spi_p_{0};
  uint32_t arduino_spi_c_{0};

  uint32_t direct_spi_clk_{0};
  uint32_t direct_spi_u_{0};
  uint32_t direct_spi_u1_{0};
  uint32_t direct_spi_p_{0};
  uint32_t direct_spi_c_{0};

  uint32_t last_fram_refresh_ms_{0};
  uint32_t last_spi_reg_log_ms_{0};

  bool fram_signature_ok_{false};
  std::string fram_signature_text_{"NOT READ"};

  bool master_valid_{false};
  bool copy_valid_{false};
  uint64_t master_total_{0};
  uint64_t copy_total_{0};

  uint32_t fram_dump_count_{0};
  bool fram_dump_last_equal_{false};
  uint32_t fram_dump_crc_a_{0};
  uint32_t fram_dump_crc_b_{0};
  uint16_t fram_dump_mismatch_count_{0};
  uint8_t fram_dump_a_[512]{};
  uint8_t fram_dump_b_[512]{};

  // V0.2 production state. Counting uses raw-byte modulo deltas; C755 is a cross-check/diagnostic.
  uint64_t total_liters_{0};
  uint64_t install_baseline_liters_{V2_INITIAL_TOTAL_L};
  uint64_t last_persisted_total_{0};
  uint8_t counter_last_raw_{0};
  bool counter_last_raw_valid_{false};
  bool counter_synced_{false};
  uint32_t counter_event_count_{0};
  uint32_t counter_last_delta_{0};
  uint32_t counter_rejected_delta_count_{0};
  uint32_t counter_ab_retry_count_{0};
  uint32_t counter_ab_unresolved_count_{0};
  uint32_t raw_rollover_count_{0};
  uint32_t last_persist_retry_ms_{0};

  // Startup synchronization / HC590 stale-output recovery state.
  bool boot_stored_raw_valid_{false};
  uint8_t boot_stored_raw_{0};
  bool startup_stale_watch_{false};
  bool startup_recovery_pending_{false};
  uint8_t startup_recovery_raw_{0};
  uint32_t startup_recovery_candidate_liters_{0};
  uint32_t startup_recovery_count_{0};
  uint32_t startup_ambiguous_adopt_count_{0};
  bool v021_field_repair_applied_{false};
  bool v023_migration_evaluated_{false};
  bool v023_auto_migration_applied_this_boot_{false};
  uint32_t last_reject_log_ms_{0};
  uint8_t last_reject_logged_raw_{0xFF};

  bool v2_storage_ready_{false};
  bool v2_slot_a_valid_{false};
  bool v2_slot_b_valid_{false};
  bool v2_active_slot_a_{false};
  bool last_persist_ok_{false};
  bool v2_repair_pending_{false};
  uint32_t v2_sequence_{0};
  uint32_t v2_write_count_{0};
  uint32_t v2_write_fault_count_{0};
  std::string v2_recovery_reason_{"NOT LOADED"};

  // V0.3.0 service-layer diagnostics. Runtime-only; counter TOTAL/baseline remain in FRAM V2.
  uint32_t service_set_count_{0};
  uint32_t service_reset_count_{0};
  uint32_t service_refused_count_{0};
  bool service_last_ok_{false};
  uint64_t service_last_requested_total_{0};
  std::string service_state_text_{"IDLE"};

  // 24h validation runtime state; intentionally RAM-only. A reboot invalidates the run.
  bool validation_started_{false};
  bool validation_running_{false};
  bool validation_complete_{false};
  bool validation_stopped_early_{false};
  bool validation_start_refused_{false};
  bool validation_waiting_fresh_{false};
  uint8_t validation_stale_raw_{0};
  uint64_t validation_liters_{0};
  uint32_t validation_start_ms_{0};
  uint32_t validation_end_elapsed_s_{0};
  uint32_t validation_last_heartbeat_ms_{0};
  uint32_t validation_last_delta_{0};
  uint32_t validation_event_count_{0};
  uint32_t validation_jump_count_{0};
  uint32_t validation_max_delta_{0};
  uint32_t validation_rollover_count_{0};
  uint32_t validation_ab_retry_count_{0};
  uint32_t validation_ab_unresolved_count_{0};
  uint32_t validation_anomaly_count_{0};
  uint32_t validation_raw255_count_{0};
  uint32_t validation_gpio_fault_start_{0};

  void service_overflow_();
  void clear_overflow_latch_();
  void hc165_parallel_load_();
  void capture_gpio_registers_();
  void update_gpio_health_();
  void update_validation_();
  void finish_validation_(bool completed_24h);
  static uint8_t decode_gpio_function_(uint32_t gpf);
  uint8_t read_hc165_arduino_();
  uint8_t read_hc165_original_spi1_();

  void update_production_counter_();
  bool snapshot_valid_for_counting_() const;
  bool service_prepare_snapshot_(uint8_t *raw);
  void service_rebaseline_runtime_(uint8_t raw);
  bool service_commit_redundant_();

  uint8_t fram_read_byte_(uint16_t address);
  void fram_read_block_(uint16_t address, uint8_t *data, size_t length);
  void fram_write_byte_(uint16_t address, uint8_t value);
  void fram_write_block_(uint16_t address, const uint8_t *data, size_t length);
  void refresh_fram_snapshot_();

  void load_v2_storage_();
  void evaluate_v023_automatic_migration_();
  bool initialize_v2_storage_();
  bool persist_v2_current_(bool force_metadata_only = false);
  bool write_v2_slot_(uint16_t address, uint32_t sequence, uint64_t total, uint64_t install_baseline,
                      uint8_t last_raw, uint8_t flags, uint32_t rollover_count, uint32_t write_count);
  bool read_v2_slot_(uint16_t address, uint8_t *record);
  bool validate_v2_slot_(const uint8_t *record) const;
  bool v2_slots_blank_();
  static bool sequence_newer_(uint32_t a, uint32_t b);
  static void write_u16_le_(uint8_t *p, uint16_t value);
  static void write_u32_le_(uint8_t *p, uint32_t value);
  static void write_u64_le_(uint8_t *p, uint64_t value);

  static uint32_t read_u32_le_(const uint8_t *p);
  static uint64_t read_u64_le_(const uint8_t *p);
  static bool validate_record_(const uint8_t *record, uint64_t *total);
  static uint32_t crc32_(const uint8_t *data, size_t length);
};

}  // namespace liw01_counter_v030
}  // namespace esphome
