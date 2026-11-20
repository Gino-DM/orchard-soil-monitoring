# Oracle APEX integration notes

These notes capture the deployment lessons from getting the MQTT → Python gateway → Oracle APEX path working. They are intentionally free of hostnames, credentials and personal paths.

## Data flow

```text
ESP32 soil node
  -> LoRa receiver
  -> Mosquitto MQTT (orchard/soil/#)
  -> Raspberry Pi gateway
  -> Oracle REST Data Services (ORDS)
  -> Oracle APEX dashboard
```

## Lessons learned

1. **Check the clock first.** A Raspberry Pi without network time can produce misleading timestamps. Confirm that system clock synchronisation is active before debugging API failures.

2. **The default `python-requests` User-Agent was blocked.** Sending a browser-like `User-Agent` allowed the ORDS module to receive the request. The gateway sets:

   ```text
   User-Agent: Mozilla/5.0 (compatible; OrchardGateway/1.0)
   ```

3. **Use the parameter source expected by the ORDS module.** The working module reads values from request headers, not from a JSON body. Header names map to PL/SQL bind variables such as `node_id`, `humidity`, `conductivity`, `ph`, `nitrogen`, `temperature`, `phosphorus`, `potassium` and `reading_ts`.

4. **Confirm the real success code.** The module returned HTTP `200`, while early gateway code expected `201`, which caused successful inserts to be logged as failures. `ORACLE_API_SUCCESS_CODE` is configurable and defaults to `200`.

5. **Log both payload and evaluation data.** `soil_data.csv` stores the readings; `soil_evaluation_log.csv` stores packet size, RSSI, SNR, duplicate status, API result, response code, latency and completeness. This made the laboratory evaluation reproducible.

## Verification checklist

```bash
tail -3 soil_data.csv
tail -3 soil_evaluation_log.csv
grep -n "user-agent" monitoring.py 2>/dev/null || grep -n "USER_AGENT" monitoring.py
grep -n "ORACLE_API_SUCCESS_CODE" monitoring.py
```
