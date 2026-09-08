/*
CHANGE INDEX (project requirement: dated, short comments)
2026-08-23  V0.1.1  Safe ESPHome bring-up; HC165/C755 + original FRAM read-only.
2026-08-25  V0.1.7  Full 512-byte FRAM double-read snapshot; original FRAM still read-only.
2026-08-26  V0.1.8  24h validation logger; controlled /CCLR START; Wi-Fi LED corrected active LOW.
2026-08-27  V0.2.0  Production TOTAL; raw-modulo pulse counter; confirmed 255->0 handling;
                        FRAM V2 A/B records with sequence+CRC+commit marker; initial TOTAL 221000 L approx.
2026-08-27  V0.2.1  Startup-clear recovery after real 105->0->1 field signature; guarded APPLY/ADOPT;
                        reject log rate limiting.
2026-08-27  V0.2.2  Fix post-APPLY stale-watch bug: auto-recovery no longer depends on event_count==0.
                        Adds guarded one-time +2 L repair for the exact V0.2.1 missed-pulse field record.
2026-08-27  V0.2.3  Remove operational recovery/repair buttons. Ambiguous boot mismatch is auto-adopted +0 L;
                        known V0.2.1 missed +2 L is migrated automatically once with persistent markers.
2026-08-29  V0.3.0  SERVICE LAYER RC1: explicit SET TOTAL / RESET COUNTER transactions.
                        Normal V0.2.3 count/startup algorithm and FRAM V2 record layout are unchanged.
                        Service SET preserves exact Since-V0.2 consumption by shifting installation baseline;
                        service RESET starts a new 0 L epoch. No HC590 /CCLR operation is used.
*/

#include "liw01_counter.h"

#include "esphome/core/log.h"

#include <esp8266_peri.h>

#include <cinttypes>
#include <cstdio>

namespace esphome {
namespace liw01_counter_v030 {

static const char *const TAG = "liw01_counter";

void LIW01Counter::setup() {
  ESP_LOGI(TAG, "LIW-01 V0.3.0 SERVICE LAYER RC1; V0.2.3 golden counting core + FRAM V2 starting");

  // Preload inactive levels before switching active-LOW control pins to outputs.
  digitalWrite(PIN_HC165_CE, HIGH);
  pinMode(PIN_HC165_CE, OUTPUT);

  digitalWrite(PIN_HC165_PL, HIGH);
  pinMode(PIN_HC165_PL, OUTPUT);

  digitalWrite(PIN_C755_CLR, HIGH);
  pinMode(PIN_C755_CLR, OUTPUT);

  pinMode(PIN_C755_Q, INPUT);

  digitalWrite(PIN_FRAM_CS, HIGH);
  pinMode(PIN_FRAM_CS, OUTPUT);

  digitalWrite(PIN_HC165_CE, HIGH);
  digitalWrite(PIN_FRAM_CS, HIGH);
  digitalWrite(PIN_HC165_PL, HIGH);
  digitalWrite(PIN_C755_CLR, HIGH);

  // Snapshot actual ESP8266 GPIO/IOMUX state after our known-safe pin setup.
  this->capture_gpio_registers_();
  this->update_gpio_health_();
  ESP_LOGI(TAG,
           "GPIO init: fn0=%u fn1=%u GPE=0x%08" PRIX32 " GPO=0x%08" PRIX32
           " GPI=0x%08" PRIX32 " GPF0=0x%08" PRIX32 " GPF1=0x%08" PRIX32,
           static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf0_)),
           static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf1_)),
           this->gpio_gpe_, this->gpio_gpo_, this->gpio_gpi_, this->gpio_gpf0_, this->gpio_gpf1_);

  // GPIO2 / HC590 /CCLR remains completely untouched except digitalRead().
  ESP_LOGI(TAG, "GPIO2 HC590 /CCLR initial level=%u (%s)",
           static_cast<unsigned>(digitalRead(PIN_HC590_CCLR)),
           digitalRead(PIN_HC590_CCLR) == HIGH ? "HIGH / inactive" : "LOW / ACTIVE CLEAR");

  // Original cold-init behavior for the C755 overflow mailbox.
  this->clear_overflow_latch_();

  // Original ZAMEL/SUPLA bytes 0x000..0x035 remain read-only forever.
  this->refresh_fram_snapshot_();

  // V0.2 uses only reserved FRAM slots 0x040..0x06F and 0x080..0x0AF.
  // If both slots are blank on first installation, initialize an approximate physical
  // meter baseline of 221000 L. Invalid non-blank slots are never auto-overwritten.
  this->load_v2_storage_();

  // Establish the current HC590 byte as a startup baseline immediately. No GPIO2 clear
  // is performed in normal V0.2 operation. Pulses while the unit is fully unpowered
  // cannot be reconstructed and are intentionally not claimed.
  this->update();
}

void LIW01Counter::loop() {
  this->service_overflow_();
}

void LIW01Counter::update() {
  this->service_overflow_();

  // A/B snapshot. If a real pulse lands between the two independent reads, one
  // transient mismatch is possible. Retry once before classifying it as unresolved.
  this->raw_arduino_ = this->read_hc165_arduino_();
  this->raw_direct_ = this->read_hc165_original_spi1_();

  if (this->raw_arduino_ != this->raw_direct_) {
    this->counter_ab_retry_count_++;
    if (this->validation_running_)
      this->validation_ab_retry_count_++;
    delayMicroseconds(500);
    this->service_overflow_();
    this->raw_arduino_ = this->read_hc165_arduino_();
    this->raw_direct_ = this->read_hc165_original_spi1_();
  }

  // Catch a rollover that may have occurred while the HC165 snapshots were read.
  this->service_overflow_();

  this->hw32_ = (this->overflow_count_ << 8) + static_cast<uint32_t>(this->raw_arduino_);

  if (this->raw_arduino_ == 0xFF || this->raw_direct_ == 0xFF)
    this->raw255_count_++;

  this->capture_gpio_registers_();
  this->update_gpio_health_();

  if (this->raw_arduino_ != this->raw_direct_) {
    this->counter_ab_unresolved_count_++;
    if (this->validation_running_) {
      this->validation_ab_unresolved_count_++;
      this->validation_anomaly_count_++;
    }
    ESP_LOGE(TAG, "COUNTER ERROR unresolved A/B mismatch A=%u B=%u ovf=%" PRIu32,
             static_cast<unsigned>(this->raw_arduino_), static_cast<unsigned>(this->raw_direct_),
             this->overflow_count_);
  }

  this->update_production_counter_();
  this->update_validation_();

  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->last_spi_reg_log_ms_) >= SPI_REG_LOG_MS) {
    this->last_spi_reg_log_ms_ = now;
    ESP_LOGI(TAG,
             "SPI REG A: CLK=0x%08" PRIX32 " U=0x%08" PRIX32 " U1=0x%08" PRIX32
             " P=0x%08" PRIX32 " C=0x%08" PRIX32,
             this->arduino_spi_clk_, this->arduino_spi_u_, this->arduino_spi_u1_,
             this->arduino_spi_p_, this->arduino_spi_c_);
    ESP_LOGI(TAG,
             "SPI REG B: CLK=0x%08" PRIX32 " U=0x%08" PRIX32 " U1=0x%08" PRIX32
             " P=0x%08" PRIX32 " C=0x%08" PRIX32,
             this->direct_spi_clk_, this->direct_spi_u_, this->direct_spi_u1_,
             this->direct_spi_p_, this->direct_spi_c_);
    ESP_LOGI(TAG,
             "GPIO REG: fn0=%u fn1=%u GPE=0x%08" PRIX32 " GPO=0x%08" PRIX32
             " GPI=0x%08" PRIX32 " GPC0=0x%08" PRIX32 " GPC1=0x%08" PRIX32
             " | PLok=%u CEok=%u MUXok=%u faults=%" PRIu32,
             static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf0_)),
             static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf1_)),
             this->gpio_gpe_, this->gpio_gpo_, this->gpio_gpi_, this->gpio_gpc0_, this->gpio_gpc1_,
             static_cast<unsigned>(this->pl_pad_ok_), static_cast<unsigned>(this->ce_pad_ok_),
             static_cast<unsigned>(this->gpio_mux_ok_), this->gpio_pad_fault_count_);
  }

  if (static_cast<uint32_t>(now - this->last_fram_refresh_ms_) >= FRAM_REFRESH_MS)
    this->refresh_fram_snapshot_();
}

void LIW01Counter::dump_config() {
  ESP_LOGCONFIG(TAG, "LIW-01 Counter V0.2.3 PRODUCTION TOTAL + FRAM V2 A/B + AUTOMATIC STARTUP RECOVERY");
  ESP_LOGCONFIG(TAG, "  Counter: HC590 raw modulo-256 deltas, 100 ms polling, max accepted delta/sample=%u",
                static_cast<unsigned>(MAX_DELTA_PER_SAMPLE));
  ESP_LOGCONFIG(TAG, "  GPIO2 /CCLR: passive/read-only in normal production operation, current=%u",
                static_cast<unsigned>(digitalRead(PIN_HC590_CCLR)));
  ESP_LOGCONFIG(TAG, "  HC165 path A: Arduino SPI 50 kHz MSB mode 2");
  ESP_LOGCONFIG(TAG, "  HC165 path B: direct SPI1 original clock 0x%08" PRIX32 " mode 2", ORIGINAL_SPI_CLOCK);
  ESP_LOGCONFIG(TAG, "  FRAM original ZAMEL area 0x000..0x035: READ ONLY");
  ESP_LOGCONFIG(TAG, "  FRAM V2 slots: A=0x%03X B=0x%03X size=%u; writes use WREN/WRITE/WRDI + read-back",
                static_cast<unsigned>(V2_SLOT_A_ADDR), static_cast<unsigned>(V2_SLOT_B_ADDR),
                static_cast<unsigned>(V2_RECORD_SIZE));
  ESP_LOGCONFIG(TAG, "  Initial physical meter baseline: %" PRIu64 " L (APPROXIMATE)", V2_INITIAL_TOTAL_L);
  ESP_LOGCONFIG(TAG, "  V2 storage: %s", this->get_v2_storage_state_text().c_str());
  ESP_LOGCONFIG(TAG, "  Production: %s", this->get_production_state_text().c_str());
  ESP_LOGCONFIG(TAG, "  Original FRAM signature: %s (%s)", this->fram_signature_text_.c_str(),
                this->fram_signature_ok_ ? "OK" : "INVALID");
  ESP_LOGCONFIG(TAG, "  Original MASTER: %s total=%s", this->master_valid_ ? "VALID" : "INVALID",
                this->get_master_total_text().c_str());
  ESP_LOGCONFIG(TAG, "  Original COPY: %s total=%s", this->copy_valid_ ? "VALID" : "INVALID",
                this->get_copy_total_text().c_str());
}

bool LIW01Counter::get_overflow_pending() const {
  return digitalRead(PIN_C755_Q) == HIGH;
}

bool LIW01Counter::get_hc590_cclr_high() const {
  return digitalRead(PIN_HC590_CCLR) == HIGH;
}

void LIW01Counter::manual_hc590_clear_test() {
  // Production safety interlock: once the persistent counter is synchronized, a
  // manual HC590 clear would invalidate its raw baseline. Bench-only clear is therefore
  // refused in V0.2 normal operation.
  if (this->v2_storage_ready_ && this->counter_synced_) {
    ESP_LOGE(TAG, "HC590 CLEAR REFUSED in V0.2 production after counter synchronization");
    return;
  }

  this->hc590_clear_pulse_verified_ = false;
  this->hc590_clear_restore_ok_ = false;
  this->hc590_clear_test_last_ok_ = false;
  // Deliberately destructive only to the volatile 8-bit HC590 counter state.
  // FRAM is not written and C755/software overflow state is not reset.
  // Safety preconditions: /CCLR must currently be HIGH and GPIO2 must not already be an output.
  this->capture_gpio_registers_();
  this->hc590_clear_gpio2_before_ = digitalRead(PIN_HC590_CCLR) == HIGH ? 1 : 0;
  this->hc590_clear_oe2_before_ = static_cast<uint8_t>((this->gpio_gpe_ >> PIN_HC590_CCLR) & 1U);
  this->hc590_clear_gpf2_before_ = GPF2;

  // Take a fresh pre-clear snapshot through both already-validated HC165 read paths.
  this->hc590_clear_raw_before_a_ = this->read_hc165_arduino_();
  this->hc590_clear_raw_before_b_ = this->read_hc165_original_spi1_();

  if (this->hc590_clear_gpio2_before_ != 1 || this->hc590_clear_oe2_before_ != 0) {
    this->hc590_clear_test_ran_ = true;
    this->hc590_clear_test_last_ok_ = false;
    this->hc590_clear_timestamp_ms_ = millis();
    ESP_LOGE(TAG,
             "HC590 CLEAR TEST REFUSED: GPIO2 precondition failed: level=%u OE=%u GPF2=0x%08" PRIX32,
             static_cast<unsigned>(this->hc590_clear_gpio2_before_),
             static_cast<unsigned>(this->hc590_clear_oe2_before_), this->hc590_clear_gpf2_before_);
    return;
  }

  const bool saved_out_latch_high = ((GPO >> PIN_HC590_CCLR) & 1U) != 0;

  // Preload LOW while GPIO2 is still high-impedance, then briefly enable output.
  // This avoids a HIGH drive phase on the active-LOW /CCLR line.
  digitalWrite(PIN_HC590_CCLR, LOW);
  pinMode(PIN_HC590_CCLR, OUTPUT);

  // V0.1.6: verify the actual ESP8266 pad level while /CCLR is actively driven LOW.
  // Keep the total LOW interval at 250 us; sample near the beginning and near the end.
  delayMicroseconds(25);
  this->hc590_clear_pad_low1_ = digitalRead(PIN_HC590_CCLR) == HIGH ? 1 : 0;
  this->hc590_clear_gpi_low1_ = static_cast<uint8_t>((GPI >> PIN_HC590_CCLR) & 1U);
  this->hc590_clear_oe2_during_ = static_cast<uint8_t>((GPE >> PIN_HC590_CCLR) & 1U);
  this->hc590_clear_gpf2_during_ = GPF2;
  delayMicroseconds(200);
  this->hc590_clear_pad_low2_ = digitalRead(PIN_HC590_CCLR) == HIGH ? 1 : 0;
  this->hc590_clear_gpi_low2_ = static_cast<uint8_t>((GPI >> PIN_HC590_CCLR) & 1U);
  delayMicroseconds(25);

  this->hc590_clear_pulse_verified_ =
      (this->hc590_clear_pad_low1_ == 0) && (this->hc590_clear_gpi_low1_ == 0) &&
      (this->hc590_clear_pad_low2_ == 0) && (this->hc590_clear_gpi_low2_ == 0) &&
      (this->hc590_clear_oe2_during_ == 1);

  // Release /CCLR first by returning the pin to input/high-impedance.
  pinMode(PIN_HC590_CCLR, INPUT);

  // Restore the previous output latch and exact IOMUX register value while the output is disabled.
  digitalWrite(PIN_HC590_CCLR, saved_out_latch_high ? HIGH : LOW);
  GPF2 = this->hc590_clear_gpf2_before_;
  delayMicroseconds(250);

  this->hc590_clear_gpio2_after_ = digitalRead(PIN_HC590_CCLR) == HIGH ? 1 : 0;
  const uint8_t oe2_after = static_cast<uint8_t>((GPE >> PIN_HC590_CCLR) & 1U);

  // HC590 /CCLR clears the internal counter, not the storage register. Therefore the
  // HC165-visible value is expected to remain unchanged until the next real PULSE_OUT edge.
  this->hc590_clear_raw_after_a_ = this->read_hc165_arduino_();
  this->hc590_clear_raw_after_b_ = this->read_hc165_original_spi1_();

  this->hc590_clear_test_count_++;
  this->hc590_clear_test_ran_ = true;
  this->hc590_clear_timestamp_ms_ = millis();
  this->hc590_clear_restore_ok_ =
      (this->hc590_clear_gpio2_after_ == 1) && (oe2_after == 0) &&
      (GPF2 == this->hc590_clear_gpf2_before_);
  this->hc590_clear_test_last_ok_ =
      this->hc590_clear_pulse_verified_ && this->hc590_clear_restore_ok_;

  ESP_LOGW(TAG,
           "HC590 MANUAL CLEAR #%" PRIu32
           ": /CCLR LOW 250us; before A/B=%u/%u GPIO2=%u OE2=%u GPF2=0x%08" PRIX32
           " | DURING pad=%u/%u gpi=%u/%u OE2=%u GPF2=0x%08" PRIX32 " pulse_ok=%u"
           " | after A/B=%u/%u GPIO2=%u OE2=%u GPF2=0x%08" PRIX32 " restore_ok=%u overall_ok=%u",
           this->hc590_clear_test_count_,
           static_cast<unsigned>(this->hc590_clear_raw_before_a_),
           static_cast<unsigned>(this->hc590_clear_raw_before_b_),
           static_cast<unsigned>(this->hc590_clear_gpio2_before_),
           static_cast<unsigned>(this->hc590_clear_oe2_before_), this->hc590_clear_gpf2_before_,
           static_cast<unsigned>(this->hc590_clear_pad_low1_),
           static_cast<unsigned>(this->hc590_clear_pad_low2_),
           static_cast<unsigned>(this->hc590_clear_gpi_low1_),
           static_cast<unsigned>(this->hc590_clear_gpi_low2_),
           static_cast<unsigned>(this->hc590_clear_oe2_during_), this->hc590_clear_gpf2_during_,
           static_cast<unsigned>(this->hc590_clear_pulse_verified_),
           static_cast<unsigned>(this->hc590_clear_raw_after_a_),
           static_cast<unsigned>(this->hc590_clear_raw_after_b_),
           static_cast<unsigned>(this->hc590_clear_gpio2_after_), static_cast<unsigned>(oe2_after),
           GPF2, static_cast<unsigned>(this->hc590_clear_restore_ok_),
           static_cast<unsigned>(this->hc590_clear_test_last_ok_));

  // Refresh public values immediately; do not touch FRAM or software overflow count.
  this->raw_arduino_ = this->hc590_clear_raw_after_a_;
  this->raw_direct_ = this->hc590_clear_raw_after_b_;
  this->hw32_ = (this->overflow_count_ << 8) + static_cast<uint32_t>(this->raw_arduino_);
}


void LIW01Counter::start_validation_24h() {
  if (this->validation_running_) {
    ESP_LOGW(TAG, "LIW24 START refused: validation already running");
    return;
  }

  this->validation_start_refused_ = false;
  this->manual_hc590_clear_test();
  if (!this->hc590_clear_test_last_ok_) {
    this->validation_start_refused_ = true;
    this->validation_started_ = false;
    this->validation_complete_ = false;
    ESP_LOGE(TAG, "LIW24 START REFUSED: verified HC590 /CCLR pulse/restore failed");
    return;
  }

  // Water must be stopped while START is pressed. Establish a deterministic volatile
  // counter epoch: HC590 internal count is zero from /CCLR, C755 Q is cleared and the
  // software overflow count begins at zero. HC590 output storage still contains a stale
  // pre-clear value until the first real PULSE_OUT edge; update_validation_ knows this.
  this->clear_overflow_latch_();
  this->overflow_count_ = 0;

  this->validation_started_ = true;
  this->validation_running_ = true;
  this->validation_complete_ = false;
  this->validation_stopped_early_ = false;
  this->validation_waiting_fresh_ = true;
  this->validation_stale_raw_ = this->raw_arduino_;
  this->validation_liters_ = 0;
  this->validation_start_ms_ = millis();
  this->validation_end_elapsed_s_ = 0;
  this->validation_last_heartbeat_ms_ = this->validation_start_ms_;
  this->validation_last_delta_ = 0;
  this->validation_event_count_ = 0;
  this->validation_jump_count_ = 0;
  this->validation_max_delta_ = 0;
  this->validation_rollover_count_ = 0;
  this->validation_ab_retry_count_ = 0;
  this->validation_ab_unresolved_count_ = 0;
  this->validation_anomaly_count_ = 0;
  this->validation_raw255_count_ = 0;
  this->validation_gpio_fault_start_ = this->gpio_pad_fault_count_;

  ESP_LOGW(TAG,
           "LIW24 START t=0 stale_raw=%u FRAM_MASTER=%s FRAM_COPY=%s duration=86400s FRAM=READ_ONLY",
           static_cast<unsigned>(this->validation_stale_raw_), this->get_master_total_text().c_str(),
           this->get_copy_total_text().c_str());
}

void LIW01Counter::stop_validation_24h() {
  if (!this->validation_running_) {
    ESP_LOGW(TAG, "LIW24 STOP ignored: validation is not running");
    return;
  }
  this->finish_validation_(false);
}

void LIW01Counter::finish_validation_(bool completed_24h) {
  if (!this->validation_running_)
    return;

  this->validation_end_elapsed_s_ = static_cast<uint32_t>(millis() - this->validation_start_ms_) / 1000UL;
  this->validation_running_ = false;
  this->validation_complete_ = completed_24h;
  this->validation_stopped_early_ = !completed_24h;

  ESP_LOGW(TAG,
           "LIW24 END result=%s elapsed=%" PRIu32 "s liters=%" PRIu64
           " events=%" PRIu32 " jumps=%" PRIu32 " max_delta=%" PRIu32
           " rollovers=%" PRIu32 " ab_retry=%" PRIu32 " ab_unresolved=%" PRIu32
           " anomalies=%" PRIu32 " gpio_faults=%" PRIu32 " raw255=%" PRIu32,
           completed_24h ? "COMPLETE_24H" : "STOPPED_EARLY", this->validation_end_elapsed_s_,
           this->validation_liters_, this->validation_event_count_, this->validation_jump_count_,
           this->validation_max_delta_, this->validation_rollover_count_, this->validation_ab_retry_count_,
           this->validation_ab_unresolved_count_, this->validation_anomaly_count_,
           this->get_validation_gpio_fault_count(), this->validation_raw255_count_);
}

void LIW01Counter::update_validation_() {
  if (!this->validation_running_)
    return;

  const uint32_t now = millis();

  // Never derive a count from an unresolved A/B snapshot or bad control-pad state.
  if (this->raw_arduino_ == this->raw_direct_ && this->pl_pad_ok_ && this->ce_pad_ok_ && this->gpio_mux_ok_) {
    const uint8_t raw = this->raw_arduino_;

    // After explicit /CCLR the HC590 storage register remains stale until a real pulse.
    // Once the first fresh state is visible, pulses since START are deterministic:
    //   raw 0 after clear = pulse 1
    //   raw 1             = pulse 2
    //   ...
    //   raw 254           = pulse 255
    //   raw 255 + serviced overflow N = exactly N*256 pulses
    //   raw 0 with overflow N          = N*256 + 1 pulses
    if (this->validation_waiting_fresh_) {
      if (raw != this->validation_stale_raw_ || this->overflow_count_ != 0)
        this->validation_waiting_fresh_ = false;
    }

    if (!this->validation_waiting_fresh_) {
      uint64_t candidate = 0;
      if (raw == 0xFF) {
        candidate = static_cast<uint64_t>(this->overflow_count_) << 8;
      } else {
        candidate = (static_cast<uint64_t>(this->overflow_count_) << 8) + static_cast<uint64_t>(raw) + 1ULL;
      }

      if (candidate >= this->validation_liters_) {
        const uint64_t delta64 = candidate - this->validation_liters_;
        if (delta64 > 0) {
          const uint32_t delta = delta64 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<uint32_t>(delta64);
          this->validation_liters_ = candidate;
          this->validation_last_delta_ = delta;
          this->validation_event_count_++;
          if (raw == 0xFF)
            this->validation_raw255_count_++;
          if (delta > 1)
            this->validation_jump_count_++;
          if (delta > this->validation_max_delta_)
            this->validation_max_delta_ = delta;

          ESP_LOGI(TAG,
                   "LIW24 EVENT t=%" PRIu32 "ms raw=%u ovf=%" PRIu32 " delta=%" PRIu32
                   " liters=%" PRIu64 " Q=%u ab_retry=%" PRIu32 " anomalies=%" PRIu32,
                   static_cast<uint32_t>(now - this->validation_start_ms_), static_cast<unsigned>(raw),
                   this->overflow_count_, delta, this->validation_liters_,
                   static_cast<unsigned>(digitalRead(PIN_C755_Q)), this->validation_ab_retry_count_,
                   this->validation_anomaly_count_);
        }
      } else {
        this->validation_anomaly_count_++;
        ESP_LOGE(TAG,
                 "LIW24 ERROR backward candidate=%" PRIu64 " current=%" PRIu64 " raw=%u ovf=%" PRIu32,
                 candidate, this->validation_liters_, static_cast<unsigned>(raw), this->overflow_count_);
      }
    }
  }

  if (static_cast<uint32_t>(now - this->validation_last_heartbeat_ms_) >= VALIDATION_HEARTBEAT_MS) {
    this->validation_last_heartbeat_ms_ = now;
    ESP_LOGI(TAG,
             "LIW24 HEARTBEAT t=%" PRIu32 "s liters=%" PRIu64 " raw=%u ovf=%" PRIu32
             " fresh=%u events=%" PRIu32 " jumps=%" PRIu32 " max_delta=%" PRIu32
             " rollovers=%" PRIu32 " ab_retry=%" PRIu32 " ab_unresolved=%" PRIu32
             " anomalies=%" PRIu32 " gpio_faults=%" PRIu32 " raw255=%" PRIu32,
             static_cast<uint32_t>(now - this->validation_start_ms_) / 1000UL, this->validation_liters_,
             static_cast<unsigned>(this->raw_arduino_), this->overflow_count_,
             static_cast<unsigned>(!this->validation_waiting_fresh_), this->validation_event_count_,
             this->validation_jump_count_, this->validation_max_delta_, this->validation_rollover_count_,
             this->validation_ab_retry_count_, this->validation_ab_unresolved_count_,
             this->validation_anomaly_count_, this->get_validation_gpio_fault_count(),
             this->validation_raw255_count_);
  }

  if (static_cast<uint32_t>(now - this->validation_start_ms_) >= VALIDATION_DURATION_MS)
    this->finish_validation_(true);
}

uint32_t LIW01Counter::get_validation_elapsed_s() const {
  if (!this->validation_started_)
    return 0;
  if (!this->validation_running_)
    return this->validation_end_elapsed_s_;
  return static_cast<uint32_t>(millis() - this->validation_start_ms_) / 1000UL;
}

uint32_t LIW01Counter::get_validation_gpio_fault_count() const {
  if (!this->validation_started_)
    return 0;
  return this->gpio_pad_fault_count_ - this->validation_gpio_fault_start_;
}

std::string LIW01Counter::get_validation_state_text() const {
  if (this->validation_start_refused_)
    return "START REFUSED - HC590 clear verification failed";
  if (!this->validation_started_)
    return "IDLE - stop water, then press START 24H VALIDATION";
  if (this->validation_running_) {
    char buf[128];
    const uint32_t s = this->get_validation_elapsed_s();
    const uint32_t h = s / 3600UL;
    const uint32_t m = (s % 3600UL) / 60UL;
    const uint32_t sec = s % 60UL;
    std::snprintf(buf, sizeof(buf), "%s %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 " | %" PRIu64 " L",
                  this->validation_waiting_fresh_ ? "ARMED" : "RUNNING", h, m, sec,
                  this->validation_liters_);
    return std::string(buf);
  }
  if (this->validation_complete_)
    return "COMPLETE 24H";
  if (this->validation_stopped_early_)
    return "STOPPED EARLY";
  return "IDLE";
}

std::string LIW01Counter::get_validation_summary_text() const {
  char buf[240];
  std::snprintf(buf, sizeof(buf),
                "%" PRIu64 "L ev=%" PRIu32 " jump=%" PRIu32 " maxd=%" PRIu32
                " roll=%" PRIu32 " ABretry=%" PRIu32 " ABbad=%" PRIu32
                " anom=%" PRIu32 " gpio=%" PRIu32 " raw255=%" PRIu32,
                this->validation_liters_, this->validation_event_count_, this->validation_jump_count_,
                this->validation_max_delta_, this->validation_rollover_count_, this->validation_ab_retry_count_,
                this->validation_ab_unresolved_count_, this->validation_anomaly_count_,
                this->get_validation_gpio_fault_count(), this->validation_raw255_count_);
  return std::string(buf);
}


bool LIW01Counter::snapshot_valid_for_counting_() const {
  return this->raw_arduino_ == this->raw_direct_ && this->pl_pad_ok_ && this->ce_pad_ok_ && this->gpio_mux_ok_;
}

void LIW01Counter::update_production_counter_() {
  if (!this->v2_storage_ready_)
    return;

  const uint32_t now = millis();

  if (!this->snapshot_valid_for_counting_()) {
    // Keep the previous accepted raw baseline. A later valid sample will include any
    // real pulses that occurred while this snapshot was rejected.
    return;
  }

  const uint8_t raw = this->raw_arduino_;

  // V0.2.3 has no operator-dependent pending state. If an old in-RAM pending flag
  // were ever observed, resolve it conservatively by adopting the current live raw +0 L.
  if (!this->counter_synced_ && this->startup_recovery_pending_) {
    this->counter_last_raw_ = raw;
    this->counter_last_raw_valid_ = true;
    this->counter_synced_ = true;
    this->startup_stale_watch_ = false;
    this->startup_recovery_pending_ = false;
    this->startup_recovery_raw_ = raw;
    this->startup_recovery_candidate_liters_ = 0;
    this->startup_ambiguous_adopt_count_++;
    this->counter_last_delta_ = 0;
    ESP_LOGW(TAG, "V023 legacy pending state auto-adopted raw=%u with +0L", static_cast<unsigned>(raw));
    this->persist_v2_current_(true);
    return;
  }

  if (!this->counter_synced_) {
    // V0.2.3 keeps the raw stored in FRAM long enough to compare it with the first
    // live HC590 snapshot. This does NOT claim continuity across a complete power loss.
    // It is used only to classify whether startup is clean or requires explicit recovery.
    if (this->boot_stored_raw_valid_) {
      const uint8_t stored = this->boot_stored_raw_;
      const uint8_t boot_delta = static_cast<uint8_t>(raw - stored);

      if (raw == stored) {
        // Strong case observed on OTA/software reboot: the HC590 output register still
        // contains the last persisted byte. Arm a one-shot watch for the newly confirmed
        // hardware behavior where the internal counter can nevertheless restart at zero.
        this->counter_last_raw_ = raw;
        this->counter_last_raw_valid_ = true;
        this->counter_synced_ = true;
        this->startup_stale_watch_ = true;
        this->counter_last_delta_ = 0;
        ESP_LOGW(TAG,
                 "V023 SYNC stored/live raw match=%u total=%" PRIu64
                 "L seq=%" PRIu32 " | startup stale-output watch ARMED",
                 static_cast<unsigned>(raw), this->total_liters_, this->v2_sequence_);
        return;
      }

      // A large backward/modulo jump already present in the FIRST live snapshot is
      // fundamentally ambiguous: it may be a true power-loss/reset, or a HC590 internal
      // clear whose stale output already advanced before our first sample. V0.2.3 removes
      // all operator APPLY/ADOPT actions. To avoid inventing water, adopt the live raw with
      // +0 L, persist that baseline, and expose a diagnostic ambiguous-adopt counter.
      if (boot_delta > MAX_DELTA_PER_SAMPLE && raw <= STARTUP_CLEAR_RECOVERY_MAX_RAW) {
        this->counter_last_raw_ = raw;
        this->counter_last_raw_valid_ = true;
        this->counter_synced_ = true;
        this->startup_stale_watch_ = false;
        this->startup_recovery_pending_ = false;
        this->startup_recovery_raw_ = raw;
        this->startup_recovery_candidate_liters_ = 0;
        this->startup_ambiguous_adopt_count_++;
        this->counter_last_delta_ = 0;
        ESP_LOGW(TAG,
                 "V023 STARTUP AMBIGUOUS stored_raw=%u live_raw=%u modulo_delta=%u -> automatic conservative ADOPT +0L",
                 static_cast<unsigned>(stored), static_cast<unsigned>(raw), static_cast<unsigned>(boot_delta));
        this->persist_v2_current_(true);
        return;
      }

      // Conservative default for any other boot mismatch: adopt the live hardware byte
      // without adding a boot-gap delta. This preserves the original V0.2.0 safety policy.
      this->counter_last_raw_ = raw;
      this->counter_last_raw_valid_ = true;
      this->counter_synced_ = true;
      this->startup_stale_watch_ = false;
      this->counter_last_delta_ = 0;
      ESP_LOGW(TAG,
               "V023 SYNC boot mismatch stored_raw=%u live_raw=%u modulo_delta=%u -> adopted live raw, +0L",
               static_cast<unsigned>(stored), static_cast<unsigned>(raw), static_cast<unsigned>(boot_delta));
      this->persist_v2_current_(true);
      return;
    }

    // First installation / no valid stored raw metadata.
    this->counter_last_raw_ = raw;
    this->counter_last_raw_valid_ = true;
    this->counter_synced_ = true;
    this->startup_stale_watch_ = false;
    this->counter_last_delta_ = 0;
    ESP_LOGW(TAG,
             "V023 SYNC raw=%u total=%" PRIu64
             "L seq=%" PRIu32 " (no stored raw metadata; adopted live raw)",
             static_cast<unsigned>(raw), this->total_liters_, this->v2_sequence_);
    this->persist_v2_current_(true);
    return;
  }

  if (raw == this->counter_last_raw_) {
    // Once the output remained stable after software boot, the stale-output watch is
    // intentionally kept armed until the first actual change arrives.
    if (this->get_unsaved_delta() > 0 &&
        static_cast<uint32_t>(now - this->last_persist_retry_ms_) >= PERSIST_RETRY_MS) {
      this->last_persist_retry_ms_ = now;
      this->persist_v2_current_(true);
    }
    return;
  }

  const uint8_t previous_raw = this->counter_last_raw_;
  const uint8_t delta = static_cast<uint8_t>(raw - previous_raw);  // modulo 256

  // V0.2.3 real-hardware recovery path. During the first V0.2.0 field test, software
  // reboot left HC590 output raw=105 stable for ~43 s, but the first water pulse changed
  // the output to 0 and the second to 1. That proves the internal HC590 counter had
  // restarted at zero while the output register remained stale. When the exact same
  // signature occurs as the FIRST movement while startup_stale_watch_ is armed, raw=N
  // means N+1 real pulses since the internal clear (same HC590 timing proven in V0.1.8).
  // V0.2.1 incorrectly also required counter_event_count_ == 0. That made the watch
  // impossible after an explicit APPLY, because APPLY itself increments event_count. V0.2.3
  // deliberately keys only on the one-shot stale_watch state. A normal +1 movement below
  // disarms the watch before any later unrelated backward sample can be auto-recovered.
  if (delta > MAX_DELTA_PER_SAMPLE && this->startup_stale_watch_ &&
      raw <= STARTUP_CLEAR_RECOVERY_MAX_RAW) {
    const uint32_t recovered = static_cast<uint32_t>(raw) + 1U;
    this->counter_last_raw_ = raw;
    this->counter_last_raw_valid_ = true;
    this->counter_last_delta_ = recovered;
    this->counter_event_count_++;
    this->startup_recovery_count_++;
    this->startup_stale_watch_ = false;
    this->total_liters_ += static_cast<uint64_t>(recovered);

    ESP_LOGW(TAG,
             "V023 STARTUP CLEAR AUTO-RECOVERY prev_stale=%u raw=%u recovered=%" PRIu32
             "L total=%" PRIu64 "L since=%" PRIu64 "L",
             static_cast<unsigned>(previous_raw), static_cast<unsigned>(raw), recovered,
             this->total_liters_, this->get_since_v020_liters());
    this->persist_v2_current_();
    return;
  }

  this->startup_stale_watch_ = false;

  // A genuine HC590 pulse advances the registered raw byte by +1 modulo 256.
  // Multiple pulses between 100 ms samples are valid, but a huge modulo delta is
  // overwhelmingly more likely to be an inconsistent snapshot/backward glitch.
  if (delta == 0 || delta > MAX_DELTA_PER_SAMPLE) {
    this->counter_rejected_delta_count_++;

    // V0.2.0 logged every 100 ms and produced hundreds of red lines for one persistent
    // bad baseline. V0.2.3 logs immediately on a changed offending raw, then at most
    // once per REJECT_LOG_RATE_MS while the same condition remains.
    const bool raw_changed_for_log = raw != this->last_reject_logged_raw_;
    const bool rate_due = static_cast<uint32_t>(now - this->last_reject_log_ms_) >= REJECT_LOG_RATE_MS;
    if (raw_changed_for_log || rate_due) {
      this->last_reject_logged_raw_ = raw;
      this->last_reject_log_ms_ = now;
      ESP_LOGE(TAG,
               "V023 REJECT raw=%u prev=%u modulo_delta=%u max=%u A/B=%u/%u ovf=%" PRIu32
               " reject_count=%" PRIu32,
               static_cast<unsigned>(raw), static_cast<unsigned>(previous_raw), static_cast<unsigned>(delta),
               static_cast<unsigned>(MAX_DELTA_PER_SAMPLE), static_cast<unsigned>(this->raw_arduino_),
               static_cast<unsigned>(this->raw_direct_), this->overflow_count_, this->counter_rejected_delta_count_);
    }
    return;
  }

  if (raw < previous_raw)
    this->raw_rollover_count_++;

  this->counter_last_raw_ = raw;
  this->counter_last_raw_valid_ = true;
  this->counter_last_delta_ = delta;
  this->counter_event_count_++;
  this->total_liters_ += static_cast<uint64_t>(delta);

  ESP_LOGI(TAG,
           "V023 EVENT raw=%u prev=%u delta=%u total=%" PRIu64 "L since=%" PRIu64
           "L raw_roll=%" PRIu32 " c755_ovf=%" PRIu32 " seq=%" PRIu32,
           static_cast<unsigned>(raw), static_cast<unsigned>(previous_raw), static_cast<unsigned>(delta),
           this->total_liters_, this->get_since_v020_liters(), this->raw_rollover_count_, this->overflow_count_,
           this->v2_sequence_);

  this->persist_v2_current_();
}


// -----------------------------------------------------------------------------
// V0.3.0 isolated service layer
// -----------------------------------------------------------------------------
// The normal V0.2.3 update_production_counter_() path above is intentionally left
// behaviorally unchanged. Service operations run only on explicit user action.
// They never clear HC590 and never write the original ZAMEL/SUPLA FRAM region.

bool LIW01Counter::service_prepare_snapshot_(uint8_t *raw) {
  if (raw == nullptr) {
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "REFUSED null snapshot target";
    return false;
  }
  if (!this->v2_storage_ready_) {
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "REFUSED FRAM V2 not ready";
    ESP_LOGE(TAG, "V030 SERVICE REFUSED: FRAM V2 not ready");
    return false;
  }

  // Run exactly one ordinary V0.2.3 update first. Any real pulse already present
  // before the service transaction is therefore handled by the validated normal path.
  this->update();

  if (!this->snapshot_valid_for_counting_()) {
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "REFUSED invalid HC165/GPIO snapshot";
    ESP_LOGE(TAG, "V030 SERVICE REFUSED: invalid snapshot A=%u B=%u PL=%u CE=%u MUX=%u",
             static_cast<unsigned>(this->raw_arduino_), static_cast<unsigned>(this->raw_direct_),
             static_cast<unsigned>(this->pl_pad_ok_), static_cast<unsigned>(this->ce_pad_ok_),
             static_cast<unsigned>(this->gpio_mux_ok_));
    return false;
  }

  *raw = this->raw_arduino_;
  return true;
}

void LIW01Counter::service_rebaseline_runtime_(uint8_t raw) {
  this->counter_last_raw_ = raw;
  this->counter_last_raw_valid_ = true;
  this->counter_synced_ = true;
  this->counter_last_delta_ = 0;

  // A deliberate service transaction establishes a known live baseline. No stale
  // startup recovery state may remain armed across that boundary.
  this->boot_stored_raw_ = raw;
  this->boot_stored_raw_valid_ = true;
  this->startup_stale_watch_ = false;
  this->startup_recovery_pending_ = false;
  this->startup_recovery_raw_ = raw;
  this->startup_recovery_candidate_liters_ = 0;
}

bool LIW01Counter::service_commit_redundant_() {
  // Commit to the alternate slot first. Once this succeeds the new epoch is durable
  // because boot recovery selects the newest sequence. Then write the peer as a second
  // generation so both slots carry the new service state. If the peer write fails the
  // first committed generation remains authoritative and the next normal persistence
  // attempt can restore redundancy.
  if (!this->persist_v2_current_(true))
    return false;

  if (!this->persist_v2_current_(true)) {
    this->service_state_text_ = "APPLIED; peer FRAM write failed";
    ESP_LOGE(TAG, "V030 SERVICE applied but peer FRAM write failed; newest generation remains valid");
  }
  return true;
}

bool LIW01Counter::service_set_total_liters(uint64_t requested_total_l) {
  this->service_last_requested_total_ = requested_total_l;

  uint8_t raw = 0;
  if (!this->service_prepare_snapshot_(&raw))
    return false;

  // Capture state AFTER the ordinary pre-service update above.
  const uint64_t old_total = this->total_liters_;
  const uint64_t old_baseline = this->install_baseline_liters_;
  const uint64_t exact_since = this->get_since_v020_liters();

  // SET TOTAL is an absolute-meter calibration, not a consumption reset. Preserve
  // Since V0.2 exactly by shifting the installation baseline by the same offset.
  if (requested_total_l < exact_since) {
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "REFUSED target below exact Since-V0.2";
    ESP_LOGE(TAG,
             "V030 SET TOTAL REFUSED requested=%" PRIu64 "L exact_since=%" PRIu64
             "L current=%" PRIu64 "L",
             requested_total_l, exact_since, old_total);
    return false;
  }

  this->total_liters_ = requested_total_l;
  this->install_baseline_liters_ = requested_total_l - exact_since;
  this->service_rebaseline_runtime_(raw);

  if (!this->service_commit_redundant_()) {
    // No service generation became durable. Restore the pre-service software total and
    // installation baseline, but keep the fresh live raw baseline to avoid resurrecting
    // a delta that the ordinary pre-service update already handled.
    this->total_liters_ = old_total;
    this->install_baseline_liters_ = old_baseline;
    this->service_rebaseline_runtime_(raw);
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "FAILED first FRAM commit; state restored";
    ESP_LOGE(TAG, "V030 SET TOTAL FAILED first FRAM commit; restored total=%" PRIu64 "L", old_total);
    return false;
  }

  this->service_set_count_++;
  this->service_last_ok_ = true;
  if (this->last_persist_ok_)
    this->service_state_text_ = "SET TOTAL OK";

  ESP_LOGW(TAG,
           "V030 SET TOTAL OK old=%" PRIu64 "L new=%" PRIu64 "L exact_since=%" PRIu64
           "L new_install_baseline=%" PRIu64 "L raw=%u seq=%" PRIu32 " writes=%" PRIu32,
           old_total, this->total_liters_, exact_since, this->install_baseline_liters_,
           static_cast<unsigned>(raw), this->v2_sequence_, this->v2_write_count_);
  return true;
}

bool LIW01Counter::service_reset_counter() {
  this->service_last_requested_total_ = 0;

  uint8_t raw = 0;
  if (!this->service_prepare_snapshot_(&raw))
    return false;

  const uint64_t old_total = this->total_liters_;
  const uint64_t old_baseline = this->install_baseline_liters_;

  // Explicit counter reset starts a new software epoch at the current physical raw
  // position. HC590 itself is NOT cleared. Pulses after this snapshot count from zero.
  this->total_liters_ = 0;
  this->install_baseline_liters_ = 0;
  this->service_rebaseline_runtime_(raw);

  if (!this->service_commit_redundant_()) {
    this->total_liters_ = old_total;
    this->install_baseline_liters_ = old_baseline;
    this->service_rebaseline_runtime_(raw);
    this->service_refused_count_++;
    this->service_last_ok_ = false;
    this->service_state_text_ = "FAILED reset FRAM commit; state restored";
    ESP_LOGE(TAG, "V030 RESET FAILED first FRAM commit; restored total=%" PRIu64 "L", old_total);
    return false;
  }

  this->service_reset_count_++;
  this->service_last_ok_ = true;
  if (this->last_persist_ok_)
    this->service_state_text_ = "RESET COUNTER OK";

  ESP_LOGW(TAG,
           "V030 RESET COUNTER OK old=%" PRIu64 "L -> 0L raw=%u seq=%" PRIu32 " writes=%" PRIu32,
           old_total, static_cast<unsigned>(raw), this->v2_sequence_, this->v2_write_count_);
  return true;
}

std::string LIW01Counter::get_v2_storage_state_text() const {
  char buf[240];
  std::snprintf(buf, sizeof(buf),
                "%s | ready=%u A=%u B=%u active=%c seq=%" PRIu32 " writes=%" PRIu32
                " faults=%" PRIu32 " persisted=%" PRIu64 "L unsaved=%" PRIu64 "L",
                this->v2_recovery_reason_.c_str(), static_cast<unsigned>(this->v2_storage_ready_),
                static_cast<unsigned>(this->v2_slot_a_valid_), static_cast<unsigned>(this->v2_slot_b_valid_),
                this->v2_active_slot_a_ ? 'A' : 'B', this->v2_sequence_, this->v2_write_count_,
                this->v2_write_fault_count_, this->last_persisted_total_, this->get_unsaved_delta());
  return std::string(buf);
}

std::string LIW01Counter::get_production_state_text() const {
  char buf[300];
  const char *sync = this->counter_synced_ ? "SYNC" : "WAIT_SYNC";
  std::snprintf(buf, sizeof(buf),
                "V0.2.3 %s total=%" PRIu64 "L (%.3fm3) since=%" PRIu64
                "L raw=%u events=%" PRIu32 " lastd=%" PRIu32 " reject=%" PRIu32
                " recover=%" PRIu32 " ambig=%" PRIu32 " migration=%u eval=%u ABretry=%" PRIu32 " ABbad=%" PRIu32,
                sync, this->total_liters_, this->get_total_m3(), this->get_since_v020_liters(),
                static_cast<unsigned>(this->raw_arduino_), this->counter_event_count_,
                this->counter_last_delta_, this->counter_rejected_delta_count_, this->startup_recovery_count_,
                this->startup_ambiguous_adopt_count_, static_cast<unsigned>(this->v021_field_repair_applied_),
                static_cast<unsigned>(this->v023_migration_evaluated_),
                this->counter_ab_retry_count_, this->counter_ab_unresolved_count_);
  return std::string(buf);
}

std::string LIW01Counter::get_startup_recovery_state_text() const {
  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "AUTO pending=0 recoveries=%" PRIu32 " ambiguous_adopts=%" PRIu32 " stale_watch=%u",
                this->startup_recovery_count_, this->startup_ambiguous_adopt_count_,
                static_cast<unsigned>(this->startup_stale_watch_));
  return std::string(buf);
}

void LIW01Counter::service_overflow_() {
  if (digitalRead(PIN_C755_Q) != HIGH)
    return;

  this->clear_overflow_latch_();
  this->overflow_count_++;
  if (this->validation_running_)
    this->validation_rollover_count_++;
  ESP_LOGD(TAG, "C755 overflow serviced -> overflow_count=%" PRIu32, this->overflow_count_);
}

void LIW01Counter::clear_overflow_latch_() {
  digitalWrite(PIN_C755_CLR, LOW);
  delayMicroseconds(250);
  digitalWrite(PIN_C755_CLR, HIGH);
}

void LIW01Counter::hc165_parallel_load_() {
  // Read GPI, not just the output latch: this tells us what level the ESP8266 pad actually sees.
  this->pl_before_ = GPIP(PIN_HC165_PL) ? 1 : 0;

  digitalWrite(PIN_HC165_PL, LOW);
  this->pl_low_imm_ = GPIP(PIN_HC165_PL) ? 1 : 0;
  delayMicroseconds(100);
  this->pl_low_end_ = GPIP(PIN_HC165_PL) ? 1 : 0;

  digitalWrite(PIN_HC165_PL, HIGH);
  this->pl_high_imm_ = GPIP(PIN_HC165_PL) ? 1 : 0;
  delayMicroseconds(100);
  this->pl_high_end_ = GPIP(PIN_HC165_PL) ? 1 : 0;

  this->capture_gpio_registers_();
  this->update_gpio_health_();
}

uint8_t LIW01Counter::decode_gpio_function_(uint32_t gpf) {
  // ESP8266 IOMUX function select is split across bits 4, 5 and 8.
  return static_cast<uint8_t>(((gpf >> 4) & 0x1U) | (((gpf >> 5) & 0x1U) << 1) |
                              (((gpf >> 8) & 0x1U) << 2));
}

void LIW01Counter::capture_gpio_registers_() {
  this->gpio_gpo_ = GPO;
  this->gpio_gpe_ = GPE;
  this->gpio_gpi_ = GPI;
  this->gpio_gpf0_ = GPF0;
  this->gpio_gpf1_ = GPF1;
  this->gpio_gpc0_ = GPC0;
  this->gpio_gpc1_ = GPC1;
}

void LIW01Counter::update_gpio_health_() {
  const bool oe0 = ((this->gpio_gpe_ >> PIN_HC165_CE) & 1U) != 0;
  const bool oe1 = ((this->gpio_gpe_ >> PIN_HC165_PL) & 1U) != 0;
  const bool mux0 = decode_gpio_function_(this->gpio_gpf0_) == 0;
  const bool mux1 = decode_gpio_function_(this->gpio_gpf1_) == 3;
  this->gpio_mux_ok_ = oe0 && oe1 && mux0 && mux1;

  // Use the settled states as pass/fail. Immediate samples are retained only for diagnostics.
  this->pl_pad_ok_ = oe1 && mux1 && this->pl_low_end_ == 0 && this->pl_high_end_ == 1;
  this->ce_pad_ok_ = oe0 && mux0 && this->ce_during_ == 0 && this->ce_high_imm_ == 1;
}

uint8_t LIW01Counter::read_hc165_arduino_() {
  this->hc165_parallel_load_();

  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE2));

  // Capture what the ESP8266 Arduino core actually programmed for this transaction.
  this->arduino_spi_clk_ = SPI1CLK;
  this->arduino_spi_u_ = SPI1U;
  this->arduino_spi_u1_ = SPI1U1;
  this->arduino_spi_p_ = SPI1P;
  this->arduino_spi_c_ = SPI1C;

  this->ce_before_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  digitalWrite(PIN_HC165_CE, LOW);
  this->ce_low_imm_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  this->wire_arduino_ = SPI.transfer(0x00);
  this->ce_during_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  digitalWrite(PIN_HC165_CE, HIGH);
  this->ce_high_imm_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  SPI.endTransaction();
  this->capture_gpio_registers_();
  const bool prev_ok = this->ce_pad_ok_;
  this->update_gpio_health_();
  if (!this->pl_pad_ok_ || !this->ce_pad_ok_ || !this->gpio_mux_ok_) {
    this->gpio_pad_fault_count_++;
    if (prev_ok || this->gpio_pad_fault_count_ <= 3) {
      ESP_LOGW(TAG, "GPIO pad fault A: %s", this->get_gpio_pad_text().c_str());
    }
  }

  return static_cast<uint8_t>(this->wire_arduino_ ^ 0xFF);
}

uint8_t LIW01Counter::read_hc165_original_spi1_() {
  this->hc165_parallel_load_();

  while (SPI1CMD & SPIBUSY) {
  }

  // Preserve the shared HSPI state so the already validated FRAM path is not changed.
  const uint32_t save_c = SPI1C;
  const uint32_t save_c1 = SPI1C1;
  const uint32_t save_c2 = SPI1C2;
  const uint32_t save_clk = SPI1CLK;
  const uint32_t save_u = SPI1U;
  const uint32_t save_u1 = SPI1U1;
  const uint32_t save_u2 = SPI1U2;
  const uint32_t save_p = SPI1P;

  // MSB first, matching the original driver.
  SPI1C &= ~(SPICWBO | SPICRBO);

  // Exact clock register recovered from the original LIW-01 firmware.
  SPI1CLK = ORIGINAL_SPI_CLOCK;

  // SPI mode 2 on ESP8266 Arduino register mapping:
  // CPOL=1 -> SPI1P bit 29; CPHA=0 -> master edge bit SPIUSME set after the
  // ESP8266 core's CPOL compensation. Only MISO phase is enabled.
  SPI1P |= (1UL << 29);
  SPI1U = SPIUMISO | SPIUSME;

  // Exactly 8 MISO bits. Command/address/dummy/MOSI phases are disabled above.
  SPI1U1 = (7UL << SPILMISO);
  SPI1U2 = 0;
  SPI1C1 = 0;
  SPI1C2 = 0;

  this->direct_spi_clk_ = SPI1CLK;
  this->direct_spi_u_ = SPI1U;
  this->direct_spi_u1_ = SPI1U1;
  this->direct_spi_p_ = SPI1P;
  this->direct_spi_c_ = SPI1C;

  SPI1W0 = 0;
  this->ce_before_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  digitalWrite(PIN_HC165_CE, LOW);
  this->ce_low_imm_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  SPI1CMD |= SPIBUSY;
  while (SPI1CMD & SPIBUSY) {
  }
  this->wire_direct_ = static_cast<uint8_t>(SPI1W0 & 0xFF);
  this->ce_during_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  digitalWrite(PIN_HC165_CE, HIGH);
  this->ce_high_imm_ = GPIP(PIN_HC165_CE) ? 1 : 0;
  this->capture_gpio_registers_();
  this->update_gpio_health_();
  if (!this->pl_pad_ok_ || !this->ce_pad_ok_ || !this->gpio_mux_ok_) {
    this->gpio_pad_fault_count_++;
    if (this->gpio_pad_fault_count_ <= 3) {
      ESP_LOGW(TAG, "GPIO pad fault B: %s", this->get_gpio_pad_text().c_str());
    }
  }

  // Restore the ESPHome/Arduino SPI hub state exactly as found.
  SPI1C = save_c;
  SPI1C1 = save_c1;
  SPI1C2 = save_c2;
  SPI1CLK = save_clk;
  SPI1U = save_u;
  SPI1U1 = save_u1;
  SPI1U2 = save_u2;
  SPI1P = save_p;

  return static_cast<uint8_t>(this->wire_direct_ ^ 0xFF);
}


void LIW01Counter::write_u16_le_(uint8_t *p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFU);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void LIW01Counter::write_u32_le_(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFFU);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

void LIW01Counter::write_u64_le_(uint8_t *p, uint64_t value) {
  write_u32_le_(p, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
  write_u32_le_(p + 4, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
}

bool LIW01Counter::sequence_newer_(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

bool LIW01Counter::read_v2_slot_(uint16_t address, uint8_t *record) {
  this->fram_read_block_(address, record, V2_RECORD_SIZE);
  return true;
}

bool LIW01Counter::validate_v2_slot_(const uint8_t *record) const {
  if (read_u32_le_(record + 0) != V2_MAGIC)
    return false;
  const uint16_t version = static_cast<uint16_t>(record[4]) | (static_cast<uint16_t>(record[5]) << 8);
  const uint16_t length = static_cast<uint16_t>(record[6]) | (static_cast<uint16_t>(record[7]) << 8);
  if (version != V2_VERSION || length != V2_RECORD_SIZE)
    return false;
  if (read_u32_le_(record + 44) != V2_COMMIT)
    return false;
  const uint32_t stored_crc = read_u32_le_(record + 40);
  return crc32_(record, 40) == stored_crc;
}

bool LIW01Counter::v2_slots_blank_() {
  uint8_t a[V2_RECORD_SIZE]{};
  uint8_t b[V2_RECORD_SIZE]{};
  this->fram_read_block_(V2_SLOT_A_ADDR, a, sizeof(a));
  this->fram_read_block_(V2_SLOT_B_ADDR, b, sizeof(b));

  bool all_zero = true;
  bool all_ff = true;
  for (size_t i = 0; i < V2_RECORD_SIZE; i++) {
    all_zero = all_zero && a[i] == 0x00 && b[i] == 0x00;
    all_ff = all_ff && a[i] == 0xFF && b[i] == 0xFF;
  }
  return all_zero || all_ff;
}

void LIW01Counter::evaluate_v023_automatic_migration_() {
  if (!this->v2_storage_ready_ || this->v023_migration_evaluated_)
    return;

  // Device-specific one-time migration for the field-tested lineage on 2026-08-27.
  // Evidence proves V0.2.1 omitted exactly two HC590 pulse edges. By the end of the
  // V0.2.2 verification log this same device had persisted 221009 L, seq=11, writes=11,
  // field-fix flag clear, while original read-only ZAMEL MASTER/COPY remained 834/834.
  // The correction is independent of later water use, so >= thresholds allow the real
  // device to advance before V0.2.3 is flashed. Fresh V0.2.3 initialization sets the
  // EVALUATED marker immediately and can therefore never acquire this historical +2 L.
  const bool device_identity = this->fram_signature_ok_ && this->master_valid_ && this->copy_valid_ &&
                               this->master_total_ == 834ULL && this->copy_total_ == 834ULL;
  const bool known_field_lineage = this->install_baseline_liters_ == 221000ULL &&
                                   this->total_liters_ >= 221009ULL &&
                                   this->v2_sequence_ >= 11U && this->v2_write_count_ >= 11U;

  this->v023_migration_evaluated_ = true;

  if (!this->v021_field_repair_applied_ && device_identity && known_field_lineage) {
    const uint64_t before = this->total_liters_;
    this->total_liters_ += 2ULL;
    this->v021_field_repair_applied_ = true;
    this->v023_auto_migration_applied_this_boot_ = true;
    ESP_LOGW(TAG,
             "V023 AUTO MIGRATION V0.2.1 +2L applied: before=%" PRIu64 "L after=%" PRIu64
             "L seq=%" PRIu32 " writes=%" PRIu32 " original=834/834",
             before, this->total_liters_, this->v2_sequence_, this->v2_write_count_);
  } else {
    ESP_LOGI(TAG,
             "V023 migration evaluated: no +2L change (already=%u identity=%u lineage=%u total=%" PRIu64
             " seq=%" PRIu32 " writes=%" PRIu32 ")",
             static_cast<unsigned>(this->v021_field_repair_applied_), static_cast<unsigned>(device_identity),
             static_cast<unsigned>(known_field_lineage), this->total_liters_, this->v2_sequence_, this->v2_write_count_);
  }

  // Persist both the decision marker and, when applicable, the +2 L correction. If this
  // write fails, total/flags remain in RAM and the normal idle retry path will retry.
  if (!this->persist_v2_current_(true)) {
    ESP_LOGE(TAG, "V023 automatic migration metadata persist failed; RAM state retained for retry");
  }
}

void LIW01Counter::load_v2_storage_() {
  uint8_t a[V2_RECORD_SIZE]{};
  uint8_t b[V2_RECORD_SIZE]{};
  this->read_v2_slot_(V2_SLOT_A_ADDR, a);
  this->read_v2_slot_(V2_SLOT_B_ADDR, b);
  this->v2_slot_a_valid_ = this->validate_v2_slot_(a);
  this->v2_slot_b_valid_ = this->validate_v2_slot_(b);
  this->counter_synced_ = false;

  const uint8_t *chosen = nullptr;
  if (this->v2_slot_a_valid_ && this->v2_slot_b_valid_) {
    const uint32_t seq_a = read_u32_le_(a + 8);
    const uint32_t seq_b = read_u32_le_(b + 8);
    this->v2_active_slot_a_ = sequence_newer_(seq_a, seq_b);
    chosen = this->v2_active_slot_a_ ? a : b;
    this->v2_recovery_reason_ = "NORMAL BOTH VALID";
  } else if (this->v2_slot_a_valid_) {
    this->v2_active_slot_a_ = true;
    chosen = a;
    this->v2_repair_pending_ = true;
    this->v2_recovery_reason_ = "RECOVERED SLOT A; B INVALID";
  } else if (this->v2_slot_b_valid_) {
    this->v2_active_slot_a_ = false;
    chosen = b;
    this->v2_repair_pending_ = true;
    this->v2_recovery_reason_ = "RECOVERED SLOT B; A INVALID";
  } else {
    if (this->v2_slots_blank_()) {
      if (this->initialize_v2_storage_())
        return;
      this->v2_recovery_reason_ = "INITIALIZATION WRITE FAILED";
    } else {
      this->v2_recovery_reason_ = "FAULT: BOTH INVALID NON-BLANK; NO AUTO-OVERWRITE";
    }
    this->v2_storage_ready_ = false;
    this->total_liters_ = V2_INITIAL_TOTAL_L;
    this->install_baseline_liters_ = V2_INITIAL_TOTAL_L;
    this->last_persisted_total_ = 0;
    this->last_persist_ok_ = false;
    ESP_LOGE(TAG, "FRAM V2 NOT READY: %s", this->v2_recovery_reason_.c_str());
    return;
  }

  this->v2_sequence_ = read_u32_le_(chosen + 8);
  this->total_liters_ = read_u64_le_(chosen + 12);
  this->install_baseline_liters_ = read_u64_le_(chosen + 20);
  this->counter_last_raw_ = chosen[28];
  this->counter_last_raw_valid_ = (chosen[29] & V2_FLAG_LAST_RAW_VALID) != 0;
  this->v021_field_repair_applied_ = (chosen[29] & V2_FLAG_V021_FIELD_REPAIR_APPLIED) != 0;
  this->v023_migration_evaluated_ = (chosen[29] & V2_FLAG_V023_MIGRATION_EVALUATED) != 0;
  this->boot_stored_raw_ = this->counter_last_raw_;
  this->boot_stored_raw_valid_ = this->counter_last_raw_valid_;
  this->startup_stale_watch_ = false;
  this->startup_recovery_pending_ = false;
  this->startup_recovery_candidate_liters_ = 0;
  this->raw_rollover_count_ = read_u32_le_(chosen + 32);
  this->v2_write_count_ = read_u32_le_(chosen + 36);
  this->last_persisted_total_ = this->total_liters_;
  this->last_persist_ok_ = true;
  this->v2_storage_ready_ = true;

  ESP_LOGW(TAG,
           "FRAM V2 LOAD %s active=%c seq=%" PRIu32 " total=%" PRIu64
           "L baseline=%" PRIu64 "L stored_raw=%u raw_valid=%u writes=%" PRIu32,
           this->v2_recovery_reason_.c_str(), this->v2_active_slot_a_ ? 'A' : 'B', this->v2_sequence_,
           this->total_liters_, this->install_baseline_liters_, static_cast<unsigned>(this->counter_last_raw_),
           static_cast<unsigned>(this->counter_last_raw_valid_), this->v2_write_count_);

  this->evaluate_v023_automatic_migration_();
}

bool LIW01Counter::initialize_v2_storage_() {
  this->total_liters_ = V2_INITIAL_TOTAL_L;
  this->install_baseline_liters_ = V2_INITIAL_TOTAL_L;
  this->counter_last_raw_ = 0;
  this->counter_last_raw_valid_ = false;
  this->v021_field_repair_applied_ = false;
  this->v023_migration_evaluated_ = true;
  this->v023_auto_migration_applied_this_boot_ = false;
  this->boot_stored_raw_ = 0;
  this->boot_stored_raw_valid_ = false;
  this->startup_stale_watch_ = false;
  this->startup_recovery_pending_ = false;
  this->startup_recovery_candidate_liters_ = 0;
  this->raw_rollover_count_ = 0;
  this->v2_write_count_ = 0;

  const uint8_t flags = V2_FLAG_INITIAL_APPROX | V2_FLAG_V023_MIGRATION_EVALUATED;
  const bool a_ok = this->write_v2_slot_(V2_SLOT_A_ADDR, 1, this->total_liters_, this->install_baseline_liters_,
                                         0, flags, 0, 1);
  this->v2_slot_a_valid_ = a_ok;

  bool b_ok = false;
  if (a_ok) {
    b_ok = this->write_v2_slot_(V2_SLOT_B_ADDR, 2, this->total_liters_, this->install_baseline_liters_,
                                0, flags, 0, 2);
    this->v2_slot_b_valid_ = b_ok;
  }

  if (b_ok) {
    this->v2_active_slot_a_ = false;
    this->v2_sequence_ = 2;
    this->v2_write_count_ = 2;
    this->v2_repair_pending_ = false;
    this->v2_recovery_reason_ = "INITIALIZED BLANK A+B @221000L APPROX";
  } else if (a_ok) {
    this->v2_active_slot_a_ = true;
    this->v2_sequence_ = 1;
    this->v2_write_count_ = 1;
    this->v2_repair_pending_ = true;
    this->v2_recovery_reason_ = "INITIALIZED SLOT A ONLY; B REPAIR PENDING";
  } else {
    this->v2_write_fault_count_++;
    this->last_persist_ok_ = false;
    return false;
  }

  this->last_persisted_total_ = this->total_liters_;
  this->last_persist_ok_ = true;
  this->v2_storage_ready_ = true;
  ESP_LOGW(TAG, "FRAM V2 %s", this->v2_recovery_reason_.c_str());
  return true;
}

bool LIW01Counter::write_v2_slot_(uint16_t address, uint32_t sequence, uint64_t total,
                                  uint64_t install_baseline, uint8_t last_raw, uint8_t flags,
                                  uint32_t rollover_count, uint32_t write_count) {
  uint8_t record[V2_RECORD_SIZE]{};
  write_u32_le_(record + 0, V2_MAGIC);
  write_u16_le_(record + 4, V2_VERSION);
  write_u16_le_(record + 6, static_cast<uint16_t>(V2_RECORD_SIZE));
  write_u32_le_(record + 8, sequence);
  write_u64_le_(record + 12, total);
  write_u64_le_(record + 20, install_baseline);
  record[28] = last_raw;
  record[29] = flags;
  write_u16_le_(record + 30, 0);
  write_u32_le_(record + 32, rollover_count);
  write_u32_le_(record + 36, write_count);
  write_u32_le_(record + 40, crc32_(record, 40));
  write_u32_le_(record + 44, V2_COMMIT);

  // Invalidate the target first. Then write payload+CRC, and commit marker LAST.
  const uint8_t invalid_commit[4] = {0, 0, 0, 0};
  this->fram_write_block_(address + 44, invalid_commit, sizeof(invalid_commit));
  this->fram_write_block_(address, record, 44);
  this->fram_write_block_(address + 44, record + 44, 4);

  uint8_t verify[V2_RECORD_SIZE]{};
  this->fram_read_block_(address, verify, sizeof(verify));
  if (!this->validate_v2_slot_(verify) || read_u32_le_(verify + 8) != sequence ||
      read_u64_le_(verify + 12) != total || read_u64_le_(verify + 20) != install_baseline) {
    ESP_LOGE(TAG,
             "FRAM V2 VERIFY FAIL slot=0x%03X seq=%" PRIu32 " total=%" PRIu64 "L",
             static_cast<unsigned>(address), sequence, total);
    return false;
  }
  return true;
}

bool LIW01Counter::persist_v2_current_(bool force_metadata_only) {
  (void) force_metadata_only;
  if (!this->v2_storage_ready_)
    return false;

  const bool target_a = !this->v2_active_slot_a_;
  const uint16_t address = target_a ? V2_SLOT_A_ADDR : V2_SLOT_B_ADDR;
  const uint32_t next_seq = this->v2_sequence_ + 1U;
  const uint32_t next_write_count = this->v2_write_count_ + 1U;
  uint8_t flags = V2_FLAG_INITIAL_APPROX;
  if (this->counter_last_raw_valid_)
    flags |= V2_FLAG_LAST_RAW_VALID;
  if (this->v021_field_repair_applied_)
    flags |= V2_FLAG_V021_FIELD_REPAIR_APPLIED;
  if (this->v023_migration_evaluated_)
    flags |= V2_FLAG_V023_MIGRATION_EVALUATED;

  const bool ok = this->write_v2_slot_(address, next_seq, this->total_liters_, this->install_baseline_liters_,
                                       this->counter_last_raw_, flags, this->raw_rollover_count_,
                                       next_write_count);
  if (!ok) {
    this->v2_write_fault_count_++;
    this->last_persist_ok_ = false;
    this->last_persist_retry_ms_ = millis();
    ESP_LOGE(TAG, "FRAM V2 PERSIST FAILED target=%c total=%" PRIu64 "L unsaved=%" PRIu64 "L",
             target_a ? 'A' : 'B', this->total_liters_, this->get_unsaved_delta());
    return false;
  }

  this->v2_active_slot_a_ = target_a;
  if (target_a)
    this->v2_slot_a_valid_ = true;
  else
    this->v2_slot_b_valid_ = true;
  this->v2_sequence_ = next_seq;
  this->v2_write_count_ = next_write_count;
  this->last_persisted_total_ = this->total_liters_;
  this->last_persist_ok_ = true;
  this->v2_repair_pending_ = false;
  this->v2_recovery_reason_ = "NORMAL";
  return true;
}

uint8_t LIW01Counter::fram_read_byte_(uint16_t address) {
  address &= 0x01FF;
  const uint8_t opcode = (address & 0x0100) ? 0x0B : 0x03;
  const uint8_t addr_lo = static_cast<uint8_t>(address & 0xFF);

  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_FRAM_CS, LOW);
  SPI.transfer(opcode);
  SPI.transfer(addr_lo);
  const uint8_t value = SPI.transfer(0x00);
  digitalWrite(PIN_FRAM_CS, HIGH);
  SPI.endTransaction();
  return value;
}

void LIW01Counter::fram_read_block_(uint16_t address, uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++)
    data[i] = this->fram_read_byte_(address + i);
}

void LIW01Counter::fram_write_byte_(uint16_t address, uint8_t value) {
  address &= 0x01FF;
  const uint8_t opcode = (address & 0x0100) ? 0x0A : 0x02;
  const uint8_t addr_lo = static_cast<uint8_t>(address & 0xFF);

  // Match the confirmed original FM25L04B byte-write protocol exactly:
  // WREN -> WRITE one byte -> WRDI. A8 is encoded in WRITE opcode bit 3.
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));

  digitalWrite(PIN_FRAM_CS, LOW);
  SPI.transfer(0x06);  // WREN
  digitalWrite(PIN_FRAM_CS, HIGH);

  digitalWrite(PIN_FRAM_CS, LOW);
  SPI.transfer(opcode);
  SPI.transfer(addr_lo);
  SPI.transfer(value);
  digitalWrite(PIN_FRAM_CS, HIGH);

  digitalWrite(PIN_FRAM_CS, LOW);
  SPI.transfer(0x04);  // WRDI
  digitalWrite(PIN_FRAM_CS, HIGH);

  SPI.endTransaction();
}

void LIW01Counter::fram_write_block_(uint16_t address, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    this->fram_write_byte_(address + i, data[i]);
    if ((i & 0x07U) == 0x07U)
      yield();
  }
}

void LIW01Counter::refresh_fram_snapshot_() {
  uint8_t signature[6]{};
  this->fram_read_block_(0, signature, sizeof(signature));

  this->fram_signature_ok_ = signature[0] == 'S' && signature[1] == 'U' && signature[2] == 'P' &&
                             signature[3] == 'L' && signature[4] == 'A' && signature[5] == 0x01;

  char sigbuf[32];
  std::snprintf(sigbuf, sizeof(sigbuf), "%c%c%c%c%c\\x%02X", signature[0], signature[1], signature[2],
                signature[3], signature[4], signature[5]);
  this->fram_signature_text_ = sigbuf;

  uint8_t master[24]{};
  uint8_t copy[24]{};
  this->fram_read_block_(6, master, sizeof(master));
  this->fram_read_block_(30, copy, sizeof(copy));

  this->master_valid_ = validate_record_(master, &this->master_total_);
  this->copy_valid_ = validate_record_(copy, &this->copy_total_);
  this->last_fram_refresh_ms_ = millis();

  ESP_LOGI(TAG, "FRAM: sig=%s/%s MASTER=%s total=%s COPY=%s total=%s",
           this->fram_signature_text_.c_str(), this->fram_signature_ok_ ? "OK" : "BAD",
           this->master_valid_ ? "VALID" : "INVALID", this->get_master_total_text().c_str(),
           this->copy_valid_ ? "VALID" : "INVALID", this->get_copy_total_text().c_str());
}

uint32_t LIW01Counter::read_u32_le_(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t LIW01Counter::read_u64_le_(const uint8_t *p) {
  const uint64_t lo = read_u32_le_(p);
  const uint64_t hi = read_u32_le_(p + 4);
  return lo | (hi << 32);
}

bool LIW01Counter::validate_record_(const uint8_t *record, uint64_t *total) {
  if (record[0] != 0xAA)
    return false;

  const uint32_t low_a = read_u32_le_(record + 8);
  const uint32_t high_a = read_u32_le_(record + 12);
  const uint32_t low_b = read_u32_le_(record + 16);
  const uint32_t high_b = read_u32_le_(record + 20);

  if (low_a != low_b || high_a != high_b)
    return false;

  *total = (static_cast<uint64_t>(high_a) << 32) | low_a;
  return true;
}


uint32_t LIW01Counter::crc32_(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; i++) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
  }
  return crc ^ 0xFFFFFFFFUL;
}

void LIW01Counter::dump_full_fram_readonly() {
  // This routine intentionally contains READ operations only; it never calls the V0.2 write helpers.
  // Two complete passes are acquired and compared before the snapshot is accepted.
  ESP_LOGW(TAG, "FRAM512 SNAPSHOT: BEGIN double-read 0x000..0x1FF (STRICT READ ONLY)");

  this->fram_read_block_(0, this->fram_dump_a_, sizeof(this->fram_dump_a_));
  yield();
  this->fram_read_block_(0, this->fram_dump_b_, sizeof(this->fram_dump_b_));
  yield();

  this->fram_dump_crc_a_ = crc32_(this->fram_dump_a_, sizeof(this->fram_dump_a_));
  this->fram_dump_crc_b_ = crc32_(this->fram_dump_b_, sizeof(this->fram_dump_b_));
  this->fram_dump_mismatch_count_ = 0;
  for (size_t i = 0; i < sizeof(this->fram_dump_a_); i++) {
    if (this->fram_dump_a_[i] != this->fram_dump_b_[i])
      this->fram_dump_mismatch_count_++;
  }
  this->fram_dump_last_equal_ = this->fram_dump_mismatch_count_ == 0;
  this->fram_dump_count_++;

  ESP_LOGW(TAG,
           "FRAM512 SNAPSHOT #%" PRIu32 ": CRC_A=0x%08" PRIX32 " CRC_B=0x%08" PRIX32
           " equal=%u mismatches=%u",
           this->fram_dump_count_, this->fram_dump_crc_a_, this->fram_dump_crc_b_,
           static_cast<unsigned>(this->fram_dump_last_equal_),
           static_cast<unsigned>(this->fram_dump_mismatch_count_));

  if (!this->fram_dump_last_equal_) {
    for (uint16_t i = 0; i < 512; i++) {
      if (this->fram_dump_a_[i] != this->fram_dump_b_[i]) {
        ESP_LOGE(TAG, "FRAM512 MISMATCH @0x%03X A=0x%02X B=0x%02X",
                 static_cast<unsigned>(i), static_cast<unsigned>(this->fram_dump_a_[i]),
                 static_cast<unsigned>(this->fram_dump_b_[i]));
      }
    }
    ESP_LOGE(TAG, "FRAM512 SNAPSHOT: REJECTED - repeat snapshot, DO NOT flash factory firmware yet");
    return;
  }

  // Deterministic machine-readable dump: 32 lines × 16 bytes = exactly 512 bytes.
  for (uint16_t base = 0; base < 512; base += 16) {
    char line[16 * 3 + 1];
    size_t pos = 0;
    for (uint8_t j = 0; j < 16; j++) {
      const int n = std::snprintf(line + pos, sizeof(line) - pos, j == 15 ? "%02X" : "%02X ",
                                  static_cast<unsigned>(this->fram_dump_a_[base + j]));
      if (n <= 0)
        break;
      pos += static_cast<size_t>(n);
    }
    ESP_LOGI(TAG, "FRAM512 0x%03X: %s", static_cast<unsigned>(base), line);
    yield();
  }

  ESP_LOGW(TAG, "FRAM512 SNAPSHOT: END #%" PRIu32 " ACCEPTED CRC32=0x%08" PRIX32,
           this->fram_dump_count_, this->fram_dump_crc_a_);
}

std::string LIW01Counter::get_fram_dump_status_text() const {
  if (this->fram_dump_count_ == 0)
    return "NOT RUN - press FRAM FULL SNAPSHOT READ ONLY";
  char buf[160];
  std::snprintf(buf, sizeof(buf), "#%" PRIu32 " equal=%u CRC_A=%08" PRIX32 " CRC_B=%08" PRIX32 " mismatch=%u",
                this->fram_dump_count_, static_cast<unsigned>(this->fram_dump_last_equal_),
                this->fram_dump_crc_a_, this->fram_dump_crc_b_,
                static_cast<unsigned>(this->fram_dump_mismatch_count_));
  return std::string(buf);
}

std::string LIW01Counter::get_hw32_hex() const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08" PRIX32, this->hw32_);
  return std::string(buf);
}

std::string LIW01Counter::get_spi_registers_text() const {
  char buf[180];
  std::snprintf(buf, sizeof(buf),
                "A CLK=%08" PRIX32 " U=%08" PRIX32 " U1=%08" PRIX32
                " | B CLK=%08" PRIX32 " U=%08" PRIX32 " U1=%08" PRIX32,
                this->arduino_spi_clk_, this->arduino_spi_u_, this->arduino_spi_u1_,
                this->direct_spi_clk_, this->direct_spi_u_, this->direct_spi_u1_);
  return std::string(buf);
}

std::string LIW01Counter::get_gpio_pad_text() const {
  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "PL %u>%u/%u>%u/%u %s | CE %u>%u/%u>%u %s | fn0=%u fn1=%u OE0=%u OE1=%u faults=%" PRIu32,
                static_cast<unsigned>(this->pl_before_), static_cast<unsigned>(this->pl_low_imm_),
                static_cast<unsigned>(this->pl_low_end_), static_cast<unsigned>(this->pl_high_imm_),
                static_cast<unsigned>(this->pl_high_end_), this->pl_pad_ok_ ? "OK" : "BAD",
                static_cast<unsigned>(this->ce_before_), static_cast<unsigned>(this->ce_low_imm_),
                static_cast<unsigned>(this->ce_during_), static_cast<unsigned>(this->ce_high_imm_),
                this->ce_pad_ok_ ? "OK" : "BAD",
                static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf0_)),
                static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf1_)),
                static_cast<unsigned>((this->gpio_gpe_ >> PIN_HC165_CE) & 1U),
                static_cast<unsigned>((this->gpio_gpe_ >> PIN_HC165_PL) & 1U),
                this->gpio_pad_fault_count_);
  return std::string(buf);
}

std::string LIW01Counter::get_gpio_registers_text() const {
  char buf[240];
  std::snprintf(buf, sizeof(buf),
                "GPE=%08" PRIX32 " GPO=%08" PRIX32 " GPI=%08" PRIX32
                " GPF0=%08" PRIX32 "(f%u) GPF1=%08" PRIX32 "(f%u) GPC0=%08" PRIX32 " GPC1=%08" PRIX32,
                this->gpio_gpe_, this->gpio_gpo_, this->gpio_gpi_,
                this->gpio_gpf0_, static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf0_)),
                this->gpio_gpf1_, static_cast<unsigned>(decode_gpio_function_(this->gpio_gpf1_)),
                this->gpio_gpc0_, this->gpio_gpc1_);
  return std::string(buf);
}

std::string LIW01Counter::get_hc590_clear_test_text() const {
  if (!this->hc590_clear_test_ran_)
    return "NOT RUN - press manual clear only with water stopped";

  char buf[220];
  if (!this->hc590_clear_test_last_ok_) {
    std::snprintf(buf, sizeof(buf),
                  "CLEAR VERIFY FAIL: pad=%u/%u gpi=%u/%u OE2=%u pulse_ok=%u restore_ok=%u",
                  static_cast<unsigned>(this->hc590_clear_pad_low1_),
                  static_cast<unsigned>(this->hc590_clear_pad_low2_),
                  static_cast<unsigned>(this->hc590_clear_gpi_low1_),
                  static_cast<unsigned>(this->hc590_clear_gpi_low2_),
                  static_cast<unsigned>(this->hc590_clear_oe2_during_),
                  static_cast<unsigned>(this->hc590_clear_pulse_verified_),
                  static_cast<unsigned>(this->hc590_clear_restore_ok_));
    return std::string(buf);
  }

  std::snprintf(buf, sizeof(buf),
                "CLEAR #%" PRIu32 " VERIFIED: PAD LOW %u/%u, A/B %u/%u -> %u/%u; now pass 1 L",
                this->hc590_clear_test_count_,
                static_cast<unsigned>(this->hc590_clear_pad_low1_),
                static_cast<unsigned>(this->hc590_clear_pad_low2_),
                static_cast<unsigned>(this->hc590_clear_raw_before_a_),
                static_cast<unsigned>(this->hc590_clear_raw_before_b_),
                static_cast<unsigned>(this->hc590_clear_raw_after_a_),
                static_cast<unsigned>(this->hc590_clear_raw_after_b_));
  return std::string(buf);
}

std::string LIW01Counter::get_master_total_text() const {
  if (!this->master_valid_)
    return "INVALID";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%" PRIu64, this->master_total_);
  return std::string(buf);
}

std::string LIW01Counter::get_copy_total_text() const {
  if (!this->copy_valid_)
    return "INVALID";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%" PRIu64, this->copy_total_);
  return std::string(buf);
}

std::string LIW01Counter::get_state_text() const {
  if (this->validation_running_)
    return this->get_validation_state_text();
  if (!this->get_hc590_cclr_high())
    return "GPIO2 /CCLR LOW";
  if (!this->fram_signature_ok_)
    return "FRAM signature invalid";
  if (!this->gpio_mux_ok_)
    return "GPIO MUX/OE FAULT";
  if (!this->pl_pad_ok_)
    return "GPIO1 /PL PAD FAULT";
  if (!this->ce_pad_ok_)
    return "GPIO0 /CE PAD FAULT";

  char buf[128];
  if (this->hc590_clear_test_ran_ && !this->hc590_clear_test_last_ok_)
    return "HC590 CLEAR PAD VERIFY FAIL";
  if (this->raw_arduino_ != this->raw_direct_) {
    std::snprintf(buf, sizeof(buf), "SPI A/B MISMATCH: Arduino=%u Direct=%u",
                  static_cast<unsigned>(this->raw_arduino_), static_cast<unsigned>(this->raw_direct_));
  } else if (this->hc590_clear_test_ran_) {
    std::snprintf(buf, sizeof(buf), "CLEAR #%" PRIu32 " armed by test: raw=%u / GPIO2 HIGH",
                  this->hc590_clear_test_count_, static_cast<unsigned>(this->raw_arduino_));
  } else {
    std::snprintf(buf, sizeof(buf), "SPI A/B agree: raw=%u / GPIO2 HIGH",
                  static_cast<unsigned>(this->raw_arduino_));
  }
  return std::string(buf);
}

}  // namespace liw01_counter_v030
}  // namespace esphome
