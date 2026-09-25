# TDS Monitoring

Monitor TDS air (DFRobot Gravity TDS Meter V1.0), suhu air (DS18B20), dan waktu (DS3231) pada ESP32 DOIT DevKit V1. Dashboard berjalan di ESP32 lewat WiFi Access Point, tanpa internet. Data disimpan di LittleFS sebagai CSV.

## Struktur

```text
project/
├── platformio.ini
├── src/
│   └── main.cpp
└── data/
    ├── index.html
    ├── style.css
    └── script.js
```

File `/config.json` dan `/data.csv` dibuat otomatis di LittleFS saat ESP32 menyala. Keduanya tidak ada di folder `data/`.

## Library

PlatformIO mengunduh library ini dari `lib_deps` saat build:

| Library | Fungsi |
| --- | --- |
| OneWire (Paul Stoffregen) | Bus 1-Wire DS18B20 |
| DallasTemperature (Miles Burton) | Pembacaan suhu °C |
| RTClib (Adafruit) | DS3231 lewat I2C |
| ArduinoJson 6 | Body dan response API |

Sudah tersedia di framework Arduino-ESP32, tidak perlu di-install terpisah: `WiFi`, `WebServer`, `LittleFS`, `Wire`.

## Pemeriksaan pin

| Sinyal | GPIO | Alasan |
| --- | --- | --- |
| TDS analog | 34 | ADC1 channel 6. Tetap valid saat WiFi aktif. ADC2 tidak dipakai karena bentrok dengan radio. GPIO 34 input-only, cocok untuk keluaran analog. |
| DS18B20 DATA | 4 | Bukan pin strapping dan bukan pin flash. |
| I2C SDA / SCL | 21 / 22 | Pin I2C bawaan ESP32. |

Tidak dipakai: GPIO 0, 2, 12, 15 (strapping), GPIO 6–11 (flash), GPIO 1 dan 3 (USB serial).

## Wiring

Semua modul memakai **3,3 V** dan **GND yang sama** dengan ESP32. Jangan memberi DS18B20 dari 5 V: jalur data akan ikut 5 V dan merusak pin ESP32.

| Modul | Pin modul | ESP32 |
| --- | --- | --- |
| TDS Meter V1.0 | VCC | 3V3 |
| TDS Meter V1.0 | GND | GND |
| TDS Meter V1.0 | A | GPIO 34 |
| DS18B20 | VCC | 3V3 |
| DS18B20 | GND | GND |
| DS18B20 | DATA | GPIO 4 |
| DS3231 | VCC | 3V3 |
| DS3231 | GND | GND |
| DS3231 | SDA | GPIO 21 |
| DS3231 | SCL | GPIO 22 |

Pasang resistor **4,7 kΩ** antara DATA DS18B20 dan 3,3 V. Modul DS3231 breakout biasanya sudah punya pull-up I2C sendiri.

Probe TDS dan DS18B20 dicelup di air yang sama supaya kompensasi suhu sesuai kondisi ukur.

## Perhitungan TDS

Rumus mengikuti DFRobot Gravity Analog TDS Sensor V1.0 (SEN0244):

1. Ambil 30 sampel, lalu median.
2. Ubah ke volt. Contoh resmi DFRobot untuk Arduino UNO membagi hasil 10-bit dengan 1024. ESP32 memakai ADC 12-bit, jadi tegangan diambil dari `analogReadMilliVolts()` (kalibrasi eFuse), referensi 3,3 V, atenuasi 11 dB. Keluaran modul adalah 0–2,3 V, aman untuk pin ESP32.
3. Kompensasi suhu DS18B20: `Vkomp = V / (1 + 0,02 × (suhu − 25))`.
4. Kurva vendor: `(133,42·V³ − 255,86·V² + 857,39·V) × 0,5`. Angka **0,5** adalah koefisien kalibrasi bawaan DFRobot (`TDS_KVALUE` di `src/main.cpp`).

Jika DS18B20 tidak terbaca, TDS dihitung pada 25 °C dan status suhu menjadi `NOT FOUND`. Keluaran analog di atas 2,4 V ditandai `error` (di luar rentang modul). Tegangan mendekati 0 V tetap dianggap tersambung, karena air murni memang menghasilkan TDS rendah. Modul analog ini tidak bisa membedakan probe tercabut dari air yang sangat bersih.

Pembacaan DS18B20 tidak mem-blokir web server. Konversi 12-bit (~750 ms) dijalankan dengan `setWaitForConversion(false)`, lalu hasilnya diambil lewat `millis()`. Interval ukur dan interval log juga memakai `millis()`, bukan `delay()`.

## WiFi

ESP32 membuat Access Point:

- SSID: `TDS-Monitor`
- Password: `12345678`
- Alamat: `http://192.168.4.1`

Ubah SSID dan password di bagian atas `src/main.cpp` jika perlu.

## API

| Method | Path | Fungsi |
| --- | --- | --- |
| GET | `/api/sensor` | TDS (ppm), suhu (°C), dan datetime RTC. Contoh: `{"tds":245,"temperature":28.5,"datetime":"25-09-2026 10:30:15"}` |
| GET | `/api/config` | `measurementInterval` dan `logInterval` dalam milidetik |
| POST | `/api/config` | Simpan kedua interval. Body JSON: `{"measurementInterval":10000,"logInterval":60000}` |
| GET | `/api/rtc` | Tanggal `DD-MM-YYYY`, jam `HH:MM:SS`, dan status RTC |
| POST | `/api/rtc` | Atur DS3231. Body: `{"date":"2026-09-25","time":"10:30:15"}` atau tanggal `DD-MM-YYYY` |
| GET | `/api/status` | LittleFS, RTC, DS18B20, TDS, dan WiFi AP |
| GET | `/api/data/info` | Jumlah baris CSV, ukuran file, dan pemakaian LittleFS |
| GET | `/api/data/download` | Mengirim file `/data.csv` yang tersimpan di LittleFS |
| DELETE | `/api/data` | Menghapus log dan menulis ulang CSV beserta header |

Interval yang diterima hanya pilihan di menu Settings. Nilai lain ditolak.

CSV `/data.csv`:

```text
Date,Time,TDS (ppm),Temperature (°C)
25-09-2026,10:00:00,245,28.5
```

Waktu baris CSV diambil dari DS3231 saat data ditulis, bukan dari jam browser. Jika RTC tidak terdeteksi, baris tidak ditulis dan ESP32 tetap jalan dengan status `NOT FOUND`.

## Upload dan menjalankan

1. Sambungkan ESP32 DOIT DevKit V1 lewat USB.
2. Build dan upload firmware:

```text
pio run -t upload
```

3. Upload halaman web ke LittleFS (wajib, terpisah dari firmware):

```text
pio run -t uploadfs
```

Di VS Code / Cursor: PlatformIO → `esp32doit-devkit-v1` → **Upload**, lalu **Upload Filesystem Image**.

4. Hubungkan HP atau laptop ke WiFi `TDS-Monitor` (password `12345678`).
5. Buka `http://192.168.4.1`.

Monitor serial 115200 baud menampilkan alamat IP dan hasil ukur.

`uploadfs` menimpa seluruh partisi LittleFS, termasuk `data.csv` dan `config.json` yang sudah ada di perangkat. Upload firmware saja tidak menghapus log.

Menu di web UI:

- **Dashboard** — TDS, suhu, jam RTC, dan status sensor. Nilai diperbarui sendiri.
- **Settings** — interval ukur, interval log, dan set jam RTC. Keduanya tersimpan setelah restart.
- **Data Management** — jumlah data, ukuran file, unduh CSV, dan hapus log (ada konfirmasi).
