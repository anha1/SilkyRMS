#include "SilkyRMS.h"

// Continuous (DMA) ADC support needs the ESP-IDF 5 driver (ESP32 Arduino core 3.x).
// On older cores begin_adc_continuous() still exists but returns false.
#if defined(ESP_PLATFORM) && __has_include(<esp_adc/adc_continuous.h>)
#define SILKY_RMS_HAS_CONTINUOUS 1
#include <esp_adc/adc_continuous.h>
#include <esp_idf_version.h>
#include <soc/soc_caps.h>

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define SILKY_ADC_FORMAT          ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define SILKY_ADC_CHANNEL(p)      ((p)->type1.channel)
#define SILKY_ADC_DATA(p)         ((p)->type1.data)
#else
#define SILKY_ADC_FORMAT          ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define SILKY_ADC_CHANNEL(p)      ((p)->type2.channel)
#define SILKY_ADC_DATA(p)         ((p)->type2.data)
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#define SILKY_ADC_ATTEN           ADC_ATTEN_DB_12
#else
#define SILKY_ADC_ATTEN           ADC_ATTEN_DB_11
#endif

// 256 conversions per DMA frame, driver pool holds 8 frames
#define SILKY_CONT_FRAME_BYTES    (256 * SOC_ADC_DIGI_DATA_BYTES_PER_CONV)
#define SILKY_CONT_POOL_BYTES     (8 * SILKY_CONT_FRAME_BYTES)
#define SILKY_CONT_READ_TIMEOUT   100
#endif

SilkyRMS::SilkyRMS(uint8_t adc_resolution_bits, double v_ref, double volts_to_amps, double ac_voltage) {
    _pin = NO_PIN;
    _resolution_bits = adc_resolution_bits;
    _read_adc = nullptr;
    _is_begin_done = false;

    _v_ref = v_ref;
    _volts_to_amps = volts_to_amps;
    _ac_voltage = ac_voltage;

    // Dynamically calculate ADC Max (e.g., 12-bit -> 4095)
    _adc_max = (double)((1 << _resolution_bits) - 1);

    _dc_offset = (_adc_max + 1.0) / 2.0;
    _ema_alpha = 0.03;
    _samples_per_yield = 199;
    _yield_delay_ms = 2;
    _last_sample_count = 0;

    _cont_handle = nullptr;
    _cont_buf = nullptr;
    _cont_buf_len = 0;
    _cont_buf_pos = 0;
    _cont_channel = 0;
    _cont_running = false;
}

SilkyRMS::~SilkyRMS() {
#ifdef SILKY_RMS_HAS_CONTINUOUS
    if (_cont_handle) {
        cont_stop();
        adc_continuous_deinit((adc_continuous_handle_t)_cont_handle);
        _cont_handle = nullptr;
    }
#endif
    free(_cont_buf);
    _cont_buf = nullptr;
}

uint32_t SilkyRMS::do_read_adc() {
    if (_read_adc) return _read_adc();
    if (_cont_handle) {
        uint32_t raw;
        // Should never fail while the DMA is running; fall back to the midpoint
        // (contributes nothing to the RMS) rather than blocking forever.
        return cont_next(&raw) ? raw : (uint32_t)lround(_dc_offset);
    }
    return analogRead(_pin);
}

void SilkyRMS::calibrate_offset(uint32_t num_samples, uint32_t warmup_delay_ms) {
    if(!_is_begin_done) {
        throw std::invalid_argument("begin_adc_native/begin_adc_external/begin_adc_continuous - please setup one of these");
    }

    uint32_t measure_samples = 0.66 * (float)num_samples;
    uint32_t warmup_samples = num_samples - measure_samples;

    // 33% throwaway readings - give the circuit some time to stabilize
    delay(warmup_delay_ms / 2);
    for (uint32_t i = 0; i < warmup_samples; i++) {
        do_read_adc();
        if (i % 1000 == 0) delay(1);
    }
    delay(warmup_delay_ms / 2);

    uint64_t sum = 0;
    for (uint32_t i = 0; i < measure_samples; i++) {
        sum += do_read_adc();
        if (i % 1000 == 0) delay(1);
    }
    _dc_offset = (double)sum / (double)measure_samples;

    cont_stop();
}

// on-board ADC
void SilkyRMS::begin_adc_native(uint8_t pin) {
    if(_is_begin_done) {
        throw std::invalid_argument("Begin is already done");
    }
    _pin = pin;
    _read_adc = nullptr;
    analogReadResolution(_resolution_bits);
    pinMode(_pin, INPUT);
    _is_begin_done = true;
}

// custom code to read external EDC
void SilkyRMS::begin_adc_external(adc_read_cb_t callback) {
    if(_is_begin_done) {
        throw std::invalid_argument("Begin is already done");
    }
    _pin = NO_PIN;
    _read_adc = callback;
    _is_begin_done = true;
}

// on-board ADC, continuous DMA mode
bool SilkyRMS::begin_adc_continuous(uint8_t pin, uint32_t sample_freq_hz) {
    if(_is_begin_done) {
        throw std::invalid_argument("Begin is already done");
    }
#ifdef SILKY_RMS_HAS_CONTINUOUS
    if (_resolution_bits != SOC_ADC_DIGI_MAX_BITWIDTH) return false;
    if (sample_freq_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW || sample_freq_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) return false;

    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_continuous_io_to_channel(pin, &unit, &channel) != ESP_OK) return false;
    if (unit != ADC_UNIT_1) return false;   // DMA mode is ADC1-only (and ADC2 conflicts with WiFi)

    uint8_t* buf = (uint8_t*)malloc(SILKY_CONT_FRAME_BYTES);
    if (!buf) return false;

    adc_continuous_handle_t handle = nullptr;
    adc_continuous_handle_cfg_t handle_cfg = {};
    handle_cfg.max_store_buf_size = SILKY_CONT_POOL_BYTES;
    handle_cfg.conv_frame_size = SILKY_CONT_FRAME_BYTES;
    if (adc_continuous_new_handle(&handle_cfg, &handle) != ESP_OK) {
        free(buf);
        return false;
    }

    adc_digi_pattern_config_t pattern = {};
    pattern.atten = SILKY_ADC_ATTEN;
    pattern.channel = channel;
    pattern.unit = ADC_UNIT_1;
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 1;
    cfg.adc_pattern = &pattern;
    cfg.sample_freq_hz = sample_freq_hz;
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.format = SILKY_ADC_FORMAT;
    if (adc_continuous_config(handle, &cfg) != ESP_OK) {
        adc_continuous_deinit(handle);
        free(buf);
        return false;
    }

    _pin = pin;
    _read_adc = nullptr;
    _cont_handle = handle;
    _cont_buf = buf;
    _cont_channel = (uint8_t)channel;
    _cont_buf_len = 0;
    _cont_buf_pos = 0;
    _cont_running = false;
    _is_begin_done = true;
    return true;
#else
    (void)pin;
    (void)sample_freq_hz;
    return false;
#endif
}

// (Re)start the DMA so a measurement never consumes stale samples left in the pool.
void SilkyRMS::cont_restart() {
#ifdef SILKY_RMS_HAS_CONTINUOUS
    if (!_cont_handle) return;
    adc_continuous_handle_t h = (adc_continuous_handle_t)_cont_handle;
    if (_cont_running) adc_continuous_stop(h);
    _cont_running = (adc_continuous_start(h) == ESP_OK);
    _cont_buf_len = 0;
    _cont_buf_pos = 0;
#endif
}

void SilkyRMS::cont_stop() {
#ifdef SILKY_RMS_HAS_CONTINUOUS
    if (!_cont_handle || !_cont_running) return;
    adc_continuous_stop((adc_continuous_handle_t)_cont_handle);
    _cont_running = false;
    _cont_buf_len = 0;
    _cont_buf_pos = 0;
#endif
}

// Blocks (the task sleeps, so RTOS/WiFi keep running) until the DMA delivers a frame.
bool SilkyRMS::cont_fill() {
#ifdef SILKY_RMS_HAS_CONTINUOUS
    if (!_cont_running) cont_restart();
    if (!_cont_running) return false;
    uint32_t got = 0;
    esp_err_t err = adc_continuous_read((adc_continuous_handle_t)_cont_handle, _cont_buf,
                                        SILKY_CONT_FRAME_BYTES, &got, SILKY_CONT_READ_TIMEOUT);
    _cont_buf_pos = 0;
    _cont_buf_len = (err == ESP_OK) ? got : 0;
    return _cont_buf_len > 0;
#else
    return false;
#endif
}

// Next sample for our channel from the DMA buffer, refilling as needed.
bool SilkyRMS::cont_next(uint32_t* out_raw) {
#ifdef SILKY_RMS_HAS_CONTINUOUS
    for (int attempts = 0; attempts < 4; ) {
        while (_cont_buf_pos + SOC_ADC_DIGI_RESULT_BYTES <= _cont_buf_len) {
            const adc_digi_output_data_t* p = (const adc_digi_output_data_t*)&_cont_buf[_cont_buf_pos];
            _cont_buf_pos += SOC_ADC_DIGI_RESULT_BYTES;
            if (SILKY_ADC_CHANNEL(p) == _cont_channel) {
                *out_raw = SILKY_ADC_DATA(p);
                return true;
            }
        }
        if (!cont_fill()) attempts++;
    }
#else
    (void)out_raw;
#endif
    return false;
}

void SilkyRMS::set_yield_params(uint32_t samples_per_yield, uint32_t yield_delay_ms) {
    _samples_per_yield = samples_per_yield;
    _yield_delay_ms = yield_delay_ms;
}

void SilkyRMS::set_ema_alpha(double alpha) { _ema_alpha = alpha; }
void SilkyRMS::set_initial_dc_offset(double offset) { _dc_offset = offset; }
double SilkyRMS::get_dc_offset() const { return _dc_offset; }
uint32_t SilkyRMS::get_last_sample_count() const { return _last_sample_count; }

void SilkyRMS::run(uint32_t window_ms, double* out_power_w, double* out_irms) {
    if(!_is_begin_done) {
        throw std::invalid_argument("begin_adc_native/begin_adc_external/begin_adc_continuous - please setup one of these");
    }

    uint32_t samples = 0;

    // Center samples around the tracked DC offset before accumulating.
    // Variance is shift-invariant, so the result is exact regardless of how
    // accurate the offset is - centering only keeps the accumulators small
    // (overflow headroom for high-resolution ADCs, less cancellation error).
    const int32_t offset = (int32_t)lround(_dc_offset);

    int64_t sum_c = 0;
    uint64_t sum_sq_c = 0;

    if (_cont_handle) {
        // DMA mode: hardware-timed, gap-free samples. The task sleeps inside
        // adc_continuous_read() while frames fill, so no manual yields needed.
        cont_restart();
        uint32_t start_time = millis();
        uint32_t raw;
        while ((millis() - start_time) < window_ms) {
            if (!cont_next(&raw)) continue;
            int32_t d = (int32_t)raw - offset;
            uint32_t a = (uint32_t)(d < 0 ? -d : d);
            sum_c += d;
            sum_sq_c += (uint64_t)a * a;
            samples++;
        }
        cont_stop();
    } else {
        // Polling mode: high-speed integer math loop, read as much ADC values as possible
        uint32_t start_time = millis();
        while ((millis() - start_time) < window_ms) {
            for (uint32_t i = 0; i < _samples_per_yield; i++) {
                int32_t d = (int32_t)do_read_adc() - offset;
                uint32_t a = (uint32_t)(d < 0 ? -d : d);
                sum_c += d;
                sum_sq_c += (uint64_t)a * a;
            }
            samples += _samples_per_yield;
            delay(_yield_delay_ms);
        }
    }

    _last_sample_count = samples;

    // Safety catch for zero samples
    if (samples == 0) {
        if (out_power_w) *out_power_w = 0.0;
        if (out_irms) *out_irms = 0.0;
        return;
    }

    // DC Offset EMA Tracker
    double sum_c_d = (double)sum_c;
    double batch_mean = (double)offset + sum_c_d / (double)samples;
    _dc_offset += (batch_mean - _dc_offset) * _ema_alpha;

    // Final Math (variance of centered samples == variance of raw samples)
    double sum_sq = (double)sum_sq_c - ((sum_c_d * sum_c_d) / (double)samples);
    if (sum_sq < 0.0) sum_sq = 0.0;

    double raw_rms = sqrt(sum_sq / (double)samples);
    double rms_volts = raw_rms * (_v_ref / _adc_max);
    double irms = rms_volts * _volts_to_amps;
    double power_w = irms * _ac_voltage;

    // Write values to the memory addresses provided by the caller
    if (out_irms) *out_irms = irms;
    if (out_power_w) *out_power_w = power_w;
}
