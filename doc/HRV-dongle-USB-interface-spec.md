# HRV Dongle USB Interface Specification

## 1. Overview

HRV Dongle 透過 ESP32-S3 原生 USB CDC 介面將 BLE 接收到的 ECG 資料與裝置狀態傳送至 PC。

- **傳輸介面**: ESP32-S3 Native USB CDC (非 UART)
- **Baud Rate**: 115200 (初始化參數，實際為 USB Full-Speed)
- **資料方向**: 雙向 (Dongle ↔ PC)
- **最大裝置數**: 4 (device_index: 0~3)
- **Byte Order**: Little-endian (packed struct, 無 padding)

## 2. 封包類型總覽

| Header | 類型 | 長度 (bytes) | 說明 |
|--------|------|:---:|------|
| `0xAA` | ECG Data Packet | 23 | ECG 原始資料封包 |
| `0xBB` | Status Packet | 23 | 裝置連線狀態事件 |
| `0xBB` | Device Info Packet | 23 | 裝置資訊 (電量/MAC/RSSI) |
| `0xCC` | Command Packet (PC→Dongle) | 23 | MAC 白名單設定命令 |
| `0xDD` | Response Packet (Dongle→PC) | 23 | MAC 白名單設定回應 |

所有封包均為 **23 bytes** 固定長度，最後 1 byte 為 XOR checksum。

## 3. ECG Data Packet (`0xAA`)

每次 BLE notify 收到 ECG 資料時即時送出，資料率取決於感測器設定 (預設 256Hz/8bit)。

### 3.1 封包結構

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xAA` | 固定標頭 |
| 1 | device_index | 1 | 0~3 | 來源裝置編號 |
| 2 | status | 1 | bitmap | 感測器狀態與封包計數 |
| 3 | code | 1 | `0x80` | 資料類型代碼 (目前固定 0x80) |
| 4~21 | data | 18 | raw | ECG 原始資料 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

### 3.2 Status 欄位 (Byte 2) 位元定義

| Bit | 說明 |
|-----|------|
| Bit 7 | Sensor On/Off (1=on, 0=off) |
| Bit 5~6 | 保留 |
| Bit 0~4 | 封包 Counter (0x00~0x1F, 循環遞增) |

### 3.3 ECG Data 欄位 (Byte 4~21) 格式

資料格式取決於感測器端的 Data Rate 設定：

| 模式 | 資料排列 | 每封包筆數 |
|------|---------|:---:|
| **256Hz / 8-bit** (預設) | 每 byte 為一筆 8-bit 資料 | 18 筆 |
| **256Hz / 16-bit** | High byte 在前, Low byte 在後 (Byte4=H, Byte5=L, ...) | 9 筆 |

### 3.4 BLE → USB 欄位對應

```
BLE Packet (20 bytes)          USB Packet (23 bytes)
========================       ============================
                               Byte 0:  Header (0xAA)
                               Byte 1:  Device Index (0~3)
Byte 0:  Status         →     Byte 2:  Status
Byte 1:  Code (0x80)    →     Byte 3:  Code
Byte 2~19: Raw Data     →     Byte 4~21: Data
                               Byte 22: XOR Checksum
```

### 3.5 範例

裝置 0, sensor on, counter=5, 8-bit 模式:

```
AA 00 85 80 [18 bytes ECG data] [XOR]
│  │  │  │   └─ Raw ECG samples
│  │  │  └───── Code = 0x80
│  │  └──────── Status: bit7=1(on), bit0-4=0x05(counter=5)
│  └─────────── Device index = 0
└────────────── Header
```

## 4. Status Packet (`0xBB`)

裝置連線狀態變更時送出。

### 4.1 封包結構

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xBB` | 固定標頭 |
| 1 | type | 1 | 見下表 | 狀態類型 |
| 2 | device_index | 1 | 0~3 | 裝置編號 |
| 3~21 | reserved | 19 | `0x00` | 保留，目前填 0 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

### 4.2 Status Type 定義

| Type 值 | 巨集名稱 | 說明 |
|:---:|------|------|
| `0x01` | `USB_STATUS_CONNECTED` | 裝置已連線 |
| `0x02` | `USB_STATUS_DISCONNECTED` | 裝置已斷線 |
| `0x03` | `USB_STATUS_ERROR` | 裝置錯誤 |
| `0x04` | `USB_STATUS_STREAMING` | 裝置開始串流 |

## 5. Device Info Packet (`0xBB`, type=`0x05`)

週期性送出 (每 5 秒)，僅在裝置處於 STREAMING 狀態時發送。

### 5.1 封包結構

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xBB` | 固定標頭 |
| 1 | type | 1 | `0x05` | Device Info 類型 |
| 2 | device_index | 1 | 0~3 | 裝置編號 |
| 3 | battery | 1 | 0~100 / `0xFF` | 電池電量百分比，`0xFF`=未知 |
| 4~9 | mac | 6 | - | BLE MAC 位址 (Big-endian, MSB first) |
| 10 | charging | 1 | 見下表 | 充電狀態 |
| 11 | rssi | 1 (signed) | dBm | BLE RSSI (負值)，0=未知 |
| 12~21 | reserved | 10 | `0x00` | 保留，目前填 0 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

### 5.2 Charging 狀態值

| 值 | 說明 |
|:---:|------|
| `0x00` | 未知 |
| `0x01` | 充電中 |
| `0x02` | 未充電 (電池供電) |

### 5.3 範例

裝置 1, 電量 75%, MAC=70:52:EA:E5:51:D1, 未充電, RSSI=-65:

```
BB 05 01 4B 70 52 EA E5 51 D1 02 BF 00 00 00 00 00 00 00 00 00 00 [XOR]
│  │  │  │  └──────────────┘  │  │
│  │  │  │  MAC (big-endian)  │  └── RSSI = -65 (0xBF as signed)
│  │  │  │                    └───── Charging = 0x02 (未充電)
│  │  │  └─────────────────────────── Battery = 75 (0x4B)
│  │  └────────────────────────────── Device index = 1
│  └───────────────────────────────── Type = 0x05 (Device Info)
└──────────────────────────────────── Header = 0xBB
```

## 6. Checksum 計算方式

所有封包類型使用相同的 XOR checksum：

```
checksum = Byte[0] XOR Byte[1] XOR ... XOR Byte[21]
```

存放於 Byte[22] (封包最後一個 byte)。

### 驗證方式

接收端對 Byte[0]~Byte[22] 全部做 XOR，結果應為 `0x00`。

## 7. 封包辨識流程 (PC 端解析建議)

```
讀取 Byte[0]:
├── 0xAA → ECG Data Packet (讀取 23 bytes)
│           → 驗證 checksum
│           → 取出 device_index, status, code, data[18]
│
├── 0xBB → 讀取 Byte[1] (type):
│   ├── 0x01~0x04 → Status Packet (讀取 23 bytes)
│   │               → 取出 type, device_index
│   │
│   └── 0x05 → Device Info Packet (讀取 23 bytes)
│               → 取出 device_index, battery, mac[6], charging, rssi
│
└── 其他 → 丟棄，繼續搜尋下一個 0xAA 或 0xBB
```

## 8. 資料傳輸頻率參考

| 項目 | 頻率 / 週期 |
|------|------------|
| ECG Data Packet | 依 BLE notify 頻率 (256Hz/8-bit 時約每 70ms 一包, 每秒約 14 包/裝置) |
| Device Info Packet | 每 5 秒 (STATUS_INTERVAL_MS)，僅限 STREAMING 狀態的裝置 |
| Status Packet | 事件驅動 (連線/斷線/錯誤時) |

## 9. MAC Whitelist Command Packet (`0xCC`, PC → Dongle)

PC 端透過 USB CDC 發送 23 bytes 命令封包設定 / 查詢 / 重置 BLE MAC 白名單。

### 9.1 封包總覽

| Cmd | Type 名稱 | 用途 |
|:---:|-----------|------|
| `0x01` | SET_MAC | 設定指定 slot (0~3) 的 MAC |
| `0x02` | QUERY_MAC | 查詢白名單 (slot=0~3 單筆, slot=0xFF 全部) |
| `0x03` | RESET_ALL | 重置全部 slot 為 `FF:FF:FF:FF:FF:FF` (萬用) |

### 9.2 SET_MAC 封包 (0x01)

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xCC` | 固定標頭 |
| 1 | cmd | 1 | `0x01` | SET_MAC |
| 2 | slot | 1 | 0~3 | 白名單 slot 編號 |
| 3~8 | mac | 6 | - | MAC 位址 (Big-endian) |
| 9~21 | reserved | 13 | `0x00` | 保留 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

`FF:FF:FF:FF:FF:FF` 表示該 slot 為萬用 (不限 MAC)。

設定成功後 Dongle 會主動斷開所有已連線裝置並重新掃描，僅連接白名單內的裝置。

### 9.3 QUERY_MAC 封包 (0x02)

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xCC` | 固定標頭 |
| 1 | cmd | 1 | `0x02` | QUERY_MAC |
| 2 | slot | 1 | 0~3 / `0xFF` | 指定 slot 或 `0xFF` 查詢全部 |
| 3~21 | reserved | 19 | `0x00` | 保留 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

- `slot=0~3`：回傳 1 筆 `0xDD` 回應
- `slot=0xFF`：回傳 4 筆 `0xDD` 回應 (slot 0~3)

### 9.4 RESET_ALL 封包 (0x03)

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xCC` | 固定標頭 |
| 1 | cmd | 1 | `0x03` | RESET_ALL |
| 2~21 | reserved | 20 | `0x00` | 保留 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

重置後 Dongle 會主動斷開所有已連線裝置並重新掃描。

### 9.5 範例

設定 slot 0 的 MAC 為 `70:52:EA:E5:51:D1`:

```
CC 01 00 70 52 EA E5 51 D1 00 00 00 00 00 00 00 00 00 00 00 00 00 [XOR]
│  │  │  └──────────────┘
│  │  │  MAC (big-endian)
│  │  └── slot = 0
│  └───── cmd = 0x01 (SET_MAC)
└──────── header = 0xCC
```

查詢全部白名單:

```
CC 02 FF 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 [XOR]
│  │  │
│  │  └── slot = 0xFF (全部)
│  └───── cmd = 0x02 (QUERY_MAC)
└──────── header = 0xCC
```

## 10. MAC Whitelist Response Packet (`0xDD`, Dongle → PC)

Dongle 收到 `0xCC` 命令後回傳 23 bytes 回應封包。

### 10.1 封包結構

| Offset | 欄位 | 大小 | 值/範圍 | 說明 |
|:---:|------|:---:|---------|------|
| 0 | header | 1 | `0xDD` | 固定標頭 |
| 1 | cmd_echo | 1 | `0x01`~`0x03` | 原命令類型回顯 |
| 2 | status | 1 | 見下表 | 執行結果 |
| 3 | slot | 1 | 0~3 | Slot 編號 |
| 4~9 | mac | 6 | - | MAC 位址 (Big-endian) |
| 10~21 | reserved | 11 | `0x00` | 保留 |
| 22 | checksum | 1 | XOR | Byte 0~21 的 XOR |

### 10.2 Status 狀態碼

| 值 | 巨集名稱 | 說明 |
|:---:|------|------|
| `0x00` | `USB_RESP_ACK` | 成功 |
| `0x01` | `USB_RESP_NACK_PARAM` | 參數錯誤 (無效 slot / 未知命令) |
| `0x02` | `USB_RESP_NACK_CHECKSUM` | Checksum 校驗失敗 |
| `0x03` | `USB_RESP_NACK_OTHER` | 其他錯誤 (NVS 寫入失敗等) |

### 10.3 範例

SET_MAC 成功回應 (slot 0, MAC=70:52:EA:E5:51:D1):

```
DD 01 00 00 70 52 EA E5 51 D1 00 00 00 00 00 00 00 00 00 00 00 00 [XOR]
│  │  │  │  └──────────────┘
│  │  │  │  MAC (big-endian)
│  │  │  └── slot = 0
│  │  └───── status = 0x00 (ACK)
│  └──────── cmd_echo = 0x01 (SET_MAC)
└─────────── header = 0xDD
```

QUERY_MAC 回應 (slot 2 為萬用):

```
DD 02 00 02 FF FF FF FF FF FF 00 00 00 00 00 00 00 00 00 00 00 00 [XOR]
│  │  │  │  └──────────────┘
│  │  │  │  MAC = FF:FF:FF:FF:FF:FF (萬用)
│  │  │  └── slot = 2
│  │  └───── status = 0x00 (ACK)
│  └──────── cmd_echo = 0x02 (QUERY_MAC)
└─────────── header = 0xDD
```

### 10.4 NVS 持久化

白名單設定存入 ESP32 NVS (Non-Volatile Storage)，斷電後保留。

- Namespace: `"whitelist"`
- Keys: `"mac0"` ~ `"mac3"`，各 6 bytes
- 預設值: `FF:FF:FF:FF:FF:FF` (萬用，不限制 MAC)

### 10.5 封包辨識流程 (更新)

```
讀取 Byte[0]:
├── 0xAA → ECG Data Packet
├── 0xBB → Status / Device Info Packet
├── 0xDD → MAC Whitelist Response Packet
│           → 讀取 23 bytes
│           → 驗證 checksum
│           → 取出 cmd_echo, status, slot, mac[6]
└── 其他 → 丟棄，繼續搜尋
```
