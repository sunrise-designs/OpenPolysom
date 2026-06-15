#include "Seeed_LDC1612.h"
#include "hr_i2c.h"
#include "sdp800.h"
#include "patient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <string>
#include <unistd.h>
extern "C" {
#include "edflib.h"
}

#define LDC1612_MFR_ID   0x5449
#define LDC1612_DEV_ID   0x3055

#define SAMPLE_RATE_HZ          50
#define SAMPLE_INTERVAL_US      (1000000 / SAMPLE_RATE_HZ)   // 20 000 µs
#define FLUSH_INTERVAL_SAMPLES  (SAMPLE_RATE_HZ * 10)        // every 10 s
#define HR_RAW_RATE_HZ          100

// Physical range for EDF: signed deviation from baseline in raw counts
#define EDF_PHYS_MIN  -1000000.0
#define EDF_PHYS_MAX   1000000.0


#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "unknown"
#endif
const char *EQUIPMENT = "OpenPolysom v0.1 (" GIT_COMMIT_HASH ")";

struct Sample { double elapsed; u32 ch0; u32 ch1; };

static volatile bool running = true;

static void on_sigint(int) { running = false; }

static void print_sample(const Sample& s) {
    printf("t=%8.2fs  CH0: %-12u  CH1: %-12u\n", s.elapsed, s.ch0, s.ch1);
}

static std::string timestamped_name(const char* ext) {
    char buf[64];
    time_t now = time(nullptr);
    strftime(buf, sizeof(buf), "ldc1612_%Y%m%d_%H%M%S", localtime(&now));
    return std::string(buf) + ext;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s              print values to stdout (default)\n"
        "  %s edf [file]   save to EDF  (default: ldc1612_YYYYMMDD_HHMMSS.edf)\n",
        prog, prog);
}

static void average_raw_bufs(const u32* ch0_raw_buf, const u32* ch1_raw_buf, u32& ch0_avg, u32& ch1_avg) {
    uint64_t sum0 = 0, sum1 = 0;
    for (int i = 0; i < SAMPLE_RATE_HZ; i++) {
        sum0 += ch0_raw_buf[i];
        sum1 += ch1_raw_buf[i];
    }
    ch0_avg = sum0 / SAMPLE_RATE_HZ;
    ch1_avg = sum1 / SAMPLE_RATE_HZ;
}

int main(int argc, char* argv[]) {
    enum class Mode { PRINT, EDF };

    PatientInfo patient = load_patient("patient.cfg");

    Mode mode = Mode::PRINT;
    std::string out_file;

    if (argc > 1) {
        if (strcmp(argv[1], "edf") == 0) {
            mode = Mode::EDF;
            if (argc > 2) out_file = argv[2];
            else          out_file = timestamped_name(".edf");
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    // ── Sensor init ───────────────────────────────────────────────────────────
    LDC1612 sensor;
    if (sensor.init("/dev/i2c-1") != NO_ERROR) {
        fprintf(stderr, "Failed to open I2C bus\n");
        return 1;
    }
    u16 mfr_id = 0, dev_id = 0;
    sensor.IIC_read_16bit(READ_MANUFACTURER_ID, &mfr_id);
    sensor.IIC_read_16bit(READ_DEVICE_ID,       &dev_id);
    if (mfr_id != LDC1612_MFR_ID) {
        fprintf(stderr, "LDC1612 not found on bus (mfr=0x%04X dev=0x%04X)\n", mfr_id, dev_id);
        return 1;
    }
    sensor.reset_sensor();
    printf("LDC1612 found  mfr=0x%04X  dev=0x%04X\n", mfr_id, dev_id);
    if (sensor.LDC1612_mutiple_channel_config() != NO_ERROR) {
        fprintf(stderr, "Sensor configuration failed\n");
        return 1;
    }

    // ── Mode-specific setup ───────────────────────────────────────────────────
    int edf_hdl = -1;
    Sdp800 sdp800;
    bool sdp_ok = sdp800.begin();
    if (!sdp_ok) {
        fprintf(stderr, "Warning: SDP800 sensor initialization failed. Pressure readings will be disabled.\n");
    }

    HrI2c hrI2c;
    bool hr_i2c_ok = false;

    if (mode == Mode::EDF) {
        edf_hdl = edfopen_file_writeonly(out_file.c_str(), EDFLIB_FILETYPE_EDFPLUS, 6);
        if (edf_hdl < 0) {
            fprintf(stderr, "edfopen_file_writeonly failed (error %d)\n", edf_hdl);
            return 1;
        }
        edf_set_birthdate(edf_hdl, patient.dob_year, patient.dob_month, patient.dob_day);
        edf_set_patientname(edf_hdl, patient.name);
        edf_set_equipment(edf_hdl, EQUIPMENT);
        struct ChannelInfo {
            const char* transducer;
            const char* label;
            int sample_rate;
            int digital_max;
            int digital_min;
            double physical_max;
            double physical_min;
            const char* physical_dim;
        };

        const ChannelInfo channels[] = {
            { "LDC1612 CH0 - thoracic", "Thoracic", SAMPLE_RATE_HZ,  32767, -32768, EDF_PHYS_MAX, EDF_PHYS_MIN, "Inductance (nH)" },
            { "LDC1612 CH1 - abdomen",  "Abdomen",  SAMPLE_RATE_HZ,  32767, -32768, EDF_PHYS_MAX, EDF_PHYS_MIN, "Inductance (nH)" },
            { "HR - Polar H9 via ESP32-S3", "HR",   1,                32767, -32768, 250.0,        0.0,          "BPM" },
            { "RR - Polar H9 via ESP32-S3", "RR",  5,                32767, -32768, 2000.0,       0.0,          "ms" },
            { "Sensirion SDP800-125P", "Flow",     SAMPLE_RATE_HZ,   32767, -32768, 1000.0,       0.0,          "Pressure" },
            { "AD8232",                "HR_Raw",   HR_RAW_RATE_HZ,   4095,  0,      4095.0,       0.0,          "ADC" }
        };

        const int num_channels = sizeof(channels) / sizeof(channels[0]);
        for (int i = 0; i < num_channels; i++) {
            edf_set_transducer(edf_hdl, i, channels[i].transducer);
            edf_set_label(edf_hdl, i, channels[i].label);
            edf_set_samplefrequency(edf_hdl, i, channels[i].sample_rate);
            edf_set_digital_maximum(edf_hdl, i, channels[i].digital_max);
            edf_set_digital_minimum(edf_hdl, i, channels[i].digital_min);
            edf_set_physical_maximum(edf_hdl, i, channels[i].physical_max);
            edf_set_physical_minimum(edf_hdl, i, channels[i].physical_min);
            edf_set_physical_dimension(edf_hdl, i, channels[i].physical_dim);
        }

        hr_i2c_ok = hrI2c.start();
        if (!hr_i2c_ok) {
            fprintf(stderr, "Warning: Failed to start I2C HR reader (AD8232); HR_Raw will be zeros\n");
        }

        printf("Recording to %s at %d Hz  HR source: ESP32-S3 I2C  (Ctrl-C to stop)\n\n",
               out_file.c_str(), SAMPLE_RATE_HZ);

    } else {
        printf("%-10s  %-12s  %-12s\n", "Time (s)", "CH0", "CH1");
        printf("----------  ------------  ------------\n");
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    signal(SIGINT, on_sigint);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (edf_hdl >= 0) {
        time_t start_time = time(nullptr);
        struct tm tm_info;
        localtime_r(&start_time, &tm_info);
        if (edf_set_startdatetime(edf_hdl, tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec) < 0) {
            fprintf(stderr, "edf_set_startdatetime failed\n");
        }
    }

    long sample_count = 0;
    long edf_records = 0;
    double ch0_buf[SAMPLE_RATE_HZ], ch1_buf[SAMPLE_RATE_HZ];
    double hr_buf[1] = { 0.0 };
    double rr_buf[5] = { 0.0 };
    double flow_buf[SAMPLE_RATE_HZ] = { 0.0 };
    double hr_raw_buf[HR_RAW_RATE_HZ] = { 0.0 };
    int rr_slot = 0;
    bool offsets_determined = false;
    u32 ch0_raw_buf[SAMPLE_RATE_HZ], ch1_raw_buf[SAMPLE_RATE_HZ];
    u32 ch0_baseline = 0, ch1_baseline = 0;
    int buf_idx = 0;

    while (running) {
        u32 ch0 = 0, ch1 = 0;
        sensor.get_channel_result(CHANNEL_0, &ch0);
        sensor.get_channel_result(CHANNEL_1, &ch1);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9;
        Sample s = {elapsed, ch0, ch1};
        sample_count++;

        double sdp_pressure_mbar = 0.0;
        if (sdp_ok) {
            if (!sdp800.readPressureMbar(sdp_pressure_mbar)) {
                fprintf(stdout, "Warning: SDP800 read failed\n");
                sdp_ok = false;
                sdp_pressure_mbar = 0.0;
            }
        }

        if (sample_count % (SAMPLE_RATE_HZ / 5) == 0) {
            int latestHr = 0;
            int latestRr = 0;
            if (hr_i2c_ok && hrI2c.getLatestHR(latestHr, latestRr)) {
                rr_buf[rr_slot] = static_cast<double>(latestRr);
            } else {
                rr_buf[rr_slot] = 0.0;
            }
            rr_slot = (rr_slot + 1) % 5;
        }

        switch (mode) {
            case Mode::PRINT:
                print_sample(s);
                printf("RR: %.1f", rr_buf[rr_slot]);
                if (sample_count % SAMPLE_RATE_HZ == 0 && sdp_ok) {
                    printf("SDP800 pressure: %.3f mbar\n", sdp_pressure_mbar);
                }
                break;

            case Mode::EDF: {
                ch0_raw_buf[buf_idx] = ch0;
                ch1_raw_buf[buf_idx] = ch1;
                flow_buf[buf_idx] = sdp_pressure_mbar;

                

                buf_idx++;
                double time_to_recalibrate = 10 * 60; // 10 minutes
                if (elapsed >= time_to_recalibrate && 
                    buf_idx == SAMPLE_RATE_HZ && 
                    !offsets_determined) {
                        average_raw_bufs(ch0_raw_buf, ch1_raw_buf, ch0_baseline, ch1_baseline);
                        printf("Baseline: CH0: %u  CH1: %u\n", ch0_baseline, ch1_baseline);
                        offsets_determined = true;
                        buf_idx = 0;
                        rr_slot = 0;
                    }
                    // Write even if we have not re-calibrated offsets. I'd
                    // rather have some data with baseline drift than no data at all.
                else if (buf_idx == SAMPLE_RATE_HZ) {
                    for (int i = 0; i < SAMPLE_RATE_HZ; i++) {
                        ch0_buf[i] = (double)ch0_raw_buf[i] - (double)ch0_baseline;
                        ch1_buf[i] = (double)ch1_raw_buf[i] - (double)ch1_baseline;
                    }

                    int latestHr = 0;
                    int latestRr = 0;
                    if (hr_i2c_ok && hrI2c.getLatestHR(latestHr, latestRr)) {
                        hr_buf[0] = static_cast<double>(latestHr);
                    } else {
                        hr_buf[0] = 0.0;
                    }
                    if (rr_slot != 0) {
                        double lastValue = rr_buf[(rr_slot + 4) % 5];
                        for (int j = rr_slot; j < 5; j++) {
                            rr_buf[j] = lastValue;
                        }
                    }

                    hrI2c.drain(hr_raw_buf, HR_RAW_RATE_HZ);

                    edfwrite_physical_samples(edf_hdl, ch0_buf);
                    edfwrite_physical_samples(edf_hdl, ch1_buf);
                    edfwrite_physical_samples(edf_hdl, hr_buf);
                    edfwrite_physical_samples(edf_hdl, rr_buf);
                    edfwrite_physical_samples(edf_hdl, flow_buf);
                    edfwrite_physical_samples(edf_hdl, hr_raw_buf);
                    buf_idx = 0;
                    rr_slot = 0;
                    edf_records++;
                    if (sample_count % FLUSH_INTERVAL_SAMPLES == 0) {
                        edfflush_file(edf_hdl, edf_records);
                        printf("t=%8.2fs  CH0: %-12u  CH1: %-12u  [flushed]\n", elapsed, ch0, ch1);
                    } else {
                        print_sample(s);
                    }
                }
                break;
            }
        }

        usleep(SAMPLE_INTERVAL_US);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    if (edf_hdl >= 0) {
        edfclose_file(edf_hdl);
    }

    printf("Done.\n");
    return 0;
}
