# Development history

The curated branch chain is intentionally linear so the repository reads as a project that grew over time rather than as a single final upload.

```text
5d740c6  First version of the prototype
   └── a091e15  phase-01-sensor-probe
          └── c03488b  phase-02-lora-mqtt
                 └── f3d4ccb  phase-03-pi-apex
                        └── <final>  main
```

## What each stage demonstrates

### `phase-01-sensor-probe`

The prototype begins with the smallest possible proof: send a Modbus request over RS485 and decode the soil sensor's registers. This establishes the sensing layer before any networking is added.

### `phase-02-lora-mqtt`

The next commit introduces the reliability problem. Readings are packed into a fixed-size LoRa packet, sequence numbers are persisted, ACKs and retries are added, and the receiver publishes deduplicated readings to MQTT.

### `phase-03-pi-apex`

The final development branch adds low-power operation and the edge-to-cloud boundary: deep-sleep soil nodes, a Raspberry Pi logger, local CSV evaluation records, APEX ingestion and a retriable pending queue.

### `main`

The final branch is the portfolio snapshot. It keeps the same code but adds polished documentation, configuration templates, pinout diagrams and integration notes.

## Scope decision

The honours project also included irrigation/pump-control work developed with a partner. That code is deliberately not part of this curated soil-monitoring repository. Keeping the repository focused makes the author's individual contribution easier to assess and avoids publishing another person's work without explicit permission and attribution.

If the irrigation code is later included, it should live in a clearly labelled `archive/partner-irrigation` branch or directory with written permission, an explicit attribution note, and the same credential-sanitisation pass applied here.
