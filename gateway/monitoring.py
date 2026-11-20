"""Raspberry Pi soil telemetry gateway.

Subscribes to MQTT soil topics, keeps a local CSV record of every reading, posts
new readings to an Oracle APEX ORDS endpoint, and retries failed posts from a
local pending queue.

Configuration is read from environment variables (a `.env` file is supported
when python-dotenv is installed). No credentials or deployment addresses are
hard-coded.
"""

import csv
import json
import os
import time
from datetime import datetime
from pathlib import Path
from threading import Lock

import requests
import paho.mqtt.client as mqtt
from apscheduler.schedulers.background import BackgroundScheduler

try:
    from dotenv import load_dotenv

    load_dotenv()
except ImportError:
    pass


# ============================================================
# CONFIGURATION
# ============================================================
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "orchard/soil/#")

STORAGE_DIR = Path(os.getenv("GATEWAY_STORAGE_DIR", "./data"))
SOIL_CSV = STORAGE_DIR / "soil_data.csv"
EVAL_CSV = STORAGE_DIR / "soil_evaluation_log.csv"
PENDING_CSV = STORAGE_DIR / "pending_soil_data.csv"

ORACLE_SOIL_API_URL = os.getenv(
    "ORACLE_SOIL_API_URL",
    "https://your-oracle-host/ords/orcharddss/orchard/sensors/soil",
)
ORACLE_API_SUCCESS_CODE = int(os.getenv("ORACLE_API_SUCCESS_CODE", "200"))
ORACLE_API_TIMEOUT = int(os.getenv("ORACLE_API_TIMEOUT", "10"))
SYNC_INTERVAL_SECONDS = int(os.getenv("SYNC_INTERVAL_SECONDS", "60"))

USER_AGENT = "Mozilla/5.0 (compatible; OrchardGateway/1.0)"

SOIL_HEADERS = [
    "Timestamp", "Node", "Sequence", "Humidity", "Temperature",
    "Conductivity", "pH", "Nitrogen", "Phosphorus", "Potassium",
    "RSSI", "SNR",
]
EVAL_HEADERS = [
    "Timestamp", "Node", "Sequence", "Packet_Size_Bytes", "RSSI",
    "SNR", "Duplicate", "MQTT_Success", "API_Success",
    "API_Response_Code", "API_Latency_ms", "Data_Complete", "Source",
]
PENDING_HEADERS = [
    "Received_At", "Node", "Sequence", "Payload_JSON", "Attempts", "Last_Error",
]

last_sequence_by_node = {}
pending_lock = Lock()


def timestamp():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ============================================================
# STORAGE
# ============================================================
def init_storage():
    STORAGE_DIR.mkdir(parents=True, exist_ok=True)

    for path, headers in (
        (SOIL_CSV, SOIL_HEADERS),
        (EVAL_CSV, EVAL_HEADERS),
        (PENDING_CSV, PENDING_HEADERS),
    ):
        if not path.is_file():
            with path.open("w", newline="", encoding="utf-8") as file:
                csv.writer(file).writerow(headers)


# ============================================================
# DATA QUALITY
# ============================================================
def check_soil_data_complete(data):
    required = [
        "node", "sequence", "humidity", "temperature", "conductivity",
        "ph", "nitrogen", "phosphorus", "potassium",
    ]
    return 1 if all(field in data and data[field] is not None for field in required) else 0


def check_soil_duplicate(data):
    node_id = data.get("node")
    sequence = data.get("sequence")

    if node_id is None or sequence is None:
        return 0

    if node_id not in last_sequence_by_node:
        last_sequence_by_node[node_id] = sequence
        return 0

    last_sequence = last_sequence_by_node[node_id]
    if sequence == last_sequence:
        return 1
    if sequence > last_sequence:
        last_sequence_by_node[node_id] = sequence
        return 0

    # Older sequence numbers are treated as stale/duplicate packets.
    return 1


# ============================================================
# ORACLE APEX INGESTION
# ============================================================
def build_apex_headers(data):
    return {
        "vnode": str(data.get("node", "")),
        "vhum": str(data.get("humidity", "")),
        "vconduct": str(data.get("conductivity", "")),
        "vph": str(data.get("ph", "")),
        "vnitro": str(data.get("nitrogen", "")),
        "vtemp": str(data.get("temperature", "")),
        "vphos": str(data.get("phosphorus", "")),
        "vpot": str(data.get("potassium", "")),
        "vts": timestamp(),
        "User-Agent": USER_AGENT,
    }


def post_reading(data):
    started = time.perf_counter()
    try:
        response = requests.post(
            ORACLE_SOIL_API_URL,
            headers=build_apex_headers(data),
            timeout=ORACLE_API_TIMEOUT,
        )
        latency_ms = round((time.perf_counter() - started) * 1000, 2)

        if response.status_code == ORACLE_API_SUCCESS_CODE:
            return True, response.status_code, latency_ms, ""
        return False, response.status_code, latency_ms, response.text[:200]

    except requests.exceptions.RequestException as error:
        latency_ms = round((time.perf_counter() - started) * 1000, 2)
        return False, "ERROR", latency_ms, str(error)[:200]


# ============================================================
# LOGGING
# ============================================================
def log_reading(
    data,
    packet_size,
    duplicate,
    mqtt_success,
    api_success,
    api_response_code,
    api_latency_ms,
    data_complete,
    source="MQTT",
):
    with SOIL_CSV.open("a", newline="", encoding="utf-8") as file:
        csv.writer(file).writerow([
            timestamp(), data.get("node"), data.get("sequence"),
            data.get("humidity"), data.get("temperature"),
            data.get("conductivity"), data.get("ph"), data.get("nitrogen"),
            data.get("phosphorus"), data.get("potassium"),
            data.get("rssi"), data.get("snr"),
        ])

    with EVAL_CSV.open("a", newline="", encoding="utf-8") as file:
        csv.writer(file).writerow([
            timestamp(), data.get("node"), data.get("sequence"), packet_size,
            data.get("rssi"), data.get("snr"), duplicate, mqtt_success,
            api_success, api_response_code, api_latency_ms, data_complete, source,
        ])


def append_pending(data, error):
    with pending_lock:
        with PENDING_CSV.open("a", newline="", encoding="utf-8") as file:
            csv.writer(file).writerow([
                timestamp(),
                data.get("node", "unknown"),
                data.get("sequence", ""),
                json.dumps(data),
                0,
                error[:200],
            ])


def retry_pending():
    """Retry failed APEX submissions from the local pending queue."""
    with pending_lock:
        if not PENDING_CSV.is_file():
            return

        with PENDING_CSV.open("r", newline="", encoding="utf-8") as file:
            rows = list(csv.DictReader(file))

        if not rows:
            return

        remaining = []

        for row in rows:
            try:
                data = json.loads(row["Payload_JSON"])
            except (KeyError, json.JSONDecodeError):
                continue

            success, code, latency, error = post_reading(data)

            if success:
                log_reading(
                    data=data,
                    packet_size=len(row["Payload_JSON"].encode("utf-8")),
                    duplicate=0,
                    mqtt_success=1,
                    api_success=1,
                    api_response_code=code,
                    api_latency_ms=latency,
                    data_complete=check_soil_data_complete(data),
                    source="RETRY",
                )
                print(
                    f"[SYNC] Delivered buffered reading "
                    f"node={data.get('node')} sequence={data.get('sequence')}"
                )
            else:
                row["Attempts"] = str(int(row.get("Attempts") or 0) + 1)
                row["Last_Error"] = error[:200]
                remaining.append(row)

        with PENDING_CSV.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=PENDING_HEADERS)
            writer.writeheader()
            writer.writerows(remaining)


# ============================================================
# MQTT
# ============================================================
def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"[MQTT] Connected to {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe(MQTT_TOPIC)
        print(f"[MQTT] Subscribed to {MQTT_TOPIC}")
    else:
        print(f"[MQTT] Connection failed: {reason_code}")


def on_message(client, userdata, message):
    try:
        data = json.loads(message.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        print(f"[MQTT ERROR] Malformed JSON on {message.topic}")
        return

    packet_size = len(message.payload)
    complete = check_soil_data_complete(data)
    duplicate = check_soil_duplicate(data)

    if duplicate:
        print(f"[MQTT] Duplicate ignored from {data.get('node', 'unknown')}")
        log_reading(
            data=data,
            packet_size=packet_size,
            duplicate=1,
            mqtt_success=1,
            api_success="N/A",
            api_response_code="N/A",
            api_latency_ms="N/A",
            data_complete=complete,
        )
        return

    success, code, latency, error = post_reading(data)

    if success:
        print(
            f"[APEX] Delivered node={data.get('node')} "
            f"sequence={data.get('sequence')} in {latency} ms"
        )
    else:
        print(f"[APEX] Failed node={data.get('node')}: {error}")
        append_pending(data, error)

    log_reading(
        data=data,
        packet_size=packet_size,
        duplicate=0,
        mqtt_success=1,
        api_success=int(success),
        api_response_code=code,
        api_latency_ms=latency,
        data_complete=complete,
    )


# ============================================================
# ENTRY POINT
# ============================================================
def main():
    init_storage()

    scheduler = BackgroundScheduler()
    scheduler.add_job(
        retry_pending,
        "interval",
        seconds=SYNC_INTERVAL_SECONDS,
        id="retry_pending",
    )
    scheduler.start()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"[GATEWAY] Storage: {STORAGE_DIR.resolve()}")
    print(f"[GATEWAY] APEX endpoint: {ORACLE_SOIL_API_URL}")

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[GATEWAY] Stopping...")
    finally:
        scheduler.shutdown(wait=False)
        client.disconnect()


if __name__ == "__main__":
    main()
