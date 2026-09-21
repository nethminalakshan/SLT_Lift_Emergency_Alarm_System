# SLT Emergency Alarm Nodes

This folder contains the three ESP32 firmware sketches used by the SLT lift
emergency-alarm nodes:

| Node | Sketch | Lifts | Static IP | Node tag |
| --- | --- | --- | --- | --- |
| Lotus | `Lotus_lift_1_2/Lotus_lift_1_2.ino` | lift1, lift2 | 192.168.1.101 | node1 |
| Duke | `Duke_lift_3_4/Duke_lift_3_4.ino` | lift3, lift4 | 192.168.1.102 | node2 |
| OTS | `OTS_lift_5_6/OTS_lift_5_6.ino` | lift5, lift6 | 192.168.1.103 | node3 |

All three sketches follow the same design:

- ESP32 controller with a W5500 Ethernet interface.
- Ethernet is preferred; WiFi is the fallback and hot-standby connection.
- Lift inputs are read from GPIO 32 and GPIO 33.
- A DS18B20 temperature sensor is connected to GPIO 15.
- Events and periodic status are sent directly to InfluxDB using HTTP line
  protocol.
- A WiFiManager portal is available for changing saved WiFi credentials.

## Important security note

The sketches currently contain the InfluxDB token in the user configuration
section. Treat that token as a secret. Do not copy it into documentation,
commits, screenshots, public repositories, or support tickets. If the token is
ever exposed, revoke it and create a replacement in InfluxDB.

## Which file to edit

Each node is maintained as a separate Arduino sketch because each node has a
different identity, IP address, lift pair, and WiFiManager access-point name.
When making a shared behavior change, apply and test the same change in all
three files.

The sketches are very similar, but line numbers can differ. Do not use a line
number from one sketch as a permanent reference for another sketch.

## User configuration

At the beginning of every sketch, edit only the section marked
`USER CONFIGURATION SECTION` when deploying a node or changing its backend:

```cpp
#define STATIC_IP   192, 168, 1, 102
#define INFLUX_HOST "your-influxdb-host"
#define INFLUX_PORT 8086
#define INFLUX_ORG "SLT"
#define INFLUX_BUCKET "Lift_Emergency_Alarm"
#define INFLUX_TOKEN "replace-with-secret-token"
#define NODE_TAG "node2"
#define NODE_NAME "Duke"
#define LIFT_A_NAME "lift3"
#define LIFT_B_NAME "lift4"
#define AP_NAME "Duke-Lift-Node-Wifi-Setting"
#define AP_PASSWORD "replace-with-access-point-password"
```

### Configuration rules

- `STATIC_IP` uses comma-separated values because it is passed to the
  `IPAddress` constructor, for example `192, 168, 1, 102`.
- Every node must have a unique static IP.
- `NODE_NAME` is the readable node name stored in the database.
- `NODE_TAG` identifies the hardware node for deployment and diagnostics.
- `LIFT_A_NAME` and `LIFT_B_NAME` become InfluxDB field names.
- Keep `INFLUX_ORG`, `INFLUX_BUCKET`, and `INFLUX_TOKEN` identical across
  nodes when they use the same InfluxDB destination.
- `AP_NAME` is the temporary WiFi access-point name shown during setup.
- `AP_PASSWORD` is the password for that temporary access point.
- Do not create variables named `AP_NAME` or `AP_PASSWORD` elsewhere.
  They are preprocessor macros. Runtime portal values use
  `AP_NAME_VALUE` and `AP_PASSWORD_VALUE` to avoid macro collisions.

## WiFiManager access-point credentials

When a node opens its WiFiManager configuration portal, connect to the
corresponding temporary access point using this table:

| Node | Access-point name (`AP_NAME`) | Access-point password (`AP_PASSWORD`) |
| --- | --- | --- |
| Lotus | `LOTUS-Lift-Node-Wifi-Setting` | `SLT_power_operation` |
| Duke | `Duke-Lift-Node-Wifi-Setting` | `SLT_power_operation` |
| OTS | `OTS-Lift-Node-Wifi-Setting` | `SLT_power_operation` |

These are configuration-portal credentials, not the credentials of the WiFi
network the node eventually joins. Store them securely and change the shared
password in all three sketches if it is ever exposed.

## Hardware connections

The pin assignments are currently the same in all three sketches.

| Function | GPIO / setting | Notes |
| --- | ---: | --- |
| Lift A input | GPIO 32 | `INPUT_PULLUP`; active LOW |
| Lift B input | GPIO 33 | `INPUT_PULLUP`; active LOW |
| DS18B20 data | GPIO 15 | OneWire temperature bus |
| Power LED | GPIO 26 | Turned ON after GPIO initialization |
| Ethernet LED | GPIO 27 | Active Ethernet or Ethernet standby indication |
| WiFi LED | GPIO 25 | Active WiFi or WiFi standby indication |
| WiFi setup button | GPIO 14 | `INPUT_PULLUP`; hold LOW for 3 seconds |
| W5500 CS | GPIO 5 | SPI chip select |
| W5500 reset | GPIO 4 | Hardware reset |
| SPI SCK | GPIO 18 | W5500 SPI clock |
| SPI MISO | GPIO 19 | W5500 SPI data |
| SPI MOSI | GPIO 23 | W5500 SPI data |

The lift inputs use internal pull-ups. A pressed/alarm input is therefore
read as LOW:

```cpp
bool pressed = !digitalRead(liftAPin);
```

The current `isLiftModuleConnected()` function always returns `true`. A plain
pushbutton circuit cannot distinguish an unpressed lift from a disconnected
wire. Real module-disconnect detection requires an additional supervisory
loop or dedicated hardware signal.

## Status values

The same integer values are used for lift and temperature state fields:

| Value | Name | Meaning |
| ---: | --- | --- |
| 10 | `STATE_ALARM` | Lift pressed, or temperature above the threshold |
| 5 | `STATE_NORMAL` | Lift not pressed and temperature normal |
| 0 | `STATE_OFFLINE` | Sensor/module unavailable or disconnected |

The high-temperature threshold is currently `40.0 C`.

`getOverallStatus()` returns alarm when any lift or the temperature is in
alarm. The overall value is currently calculated for serial diagnostics; the
heartbeat payload stores the individual lift values and temperature.

## InfluxDB data model

All status data uses one measurement:

```text
node_status
```

The node name is stored as the `node` tag. Lift names are fields:

```text
node_status,node=Duke temp=29.50,lift3=5i,lift4=10i
node_status,node=Lotus temp=30.10,lift1=5i,lift2=5i
node_status,node=OTS temp=28.75,lift5=0i,lift6=5i
```

The examples above are illustrative. The actual temperature and state values
come from the sensors and inputs at runtime.

### Event payload

When a lift changes state, the node sends a compact event containing the
changed lift:

```text
node_status,node=Duke lift3=10i
```

### Heartbeat payload

Every 3 seconds, the node reads both lift inputs and the DS18B20, then sends a
complete node status:

```text
node_status,node=Duke temp=29.50,lift3=5i,lift4=5i
```

If the temperature sensor is unavailable, the temperature field is sent as
`0.00` and the temperature status is treated as offline for local state
evaluation.

The HTTP request is sent to:

```text
/api/v2/write?org=<INFLUX_ORG>&bucket=<INFLUX_BUCKET>&precision=s
```

A response containing HTTP `204` is considered a successful write.

## Network behavior

The connection state machine has four modes:

| Mode | Meaning |
| --- | --- |
| `MODE_ETHERNET` | Ethernet is active; WiFi may be connecting as standby |
| `MODE_DUAL` | Ethernet is active and WiFi is connected as hot standby |
| `MODE_WIFI_CONNECTING` | Ethernet is unavailable and WiFi is connecting |
| `MODE_WIFI` | WiFi is active because Ethernet is unavailable or slower |

### Boot sequence

1. Reset and initialize the W5500.
2. Check for an Ethernet link for a bounded period.
3. Try Ethernet DHCP.
4. If DHCP times out, apply the configured static IP.
5. If Ethernet is usable, start WiFi in the background as hot standby.
6. If Ethernet is unavailable, try saved WiFi credentials.
7. Continue into `loop()` even if neither connection is ready; background
   retries continue without permanently blocking the application.

The boot timing limits are intentionally bounded so a missing Ethernet cable
does not prevent the node from starting:

- Ethernet link check: 1.5 seconds.
- DHCP attempt: 4 seconds.
- DHCP response window: 1 second.
- Initial WiFi connection: 5 seconds.

### Selecting the preferred connection

When both interfaces are connected, the firmware opens a TCP connection to
`INFLUX_HOST:INFLUX_PORT` through each interface and compares connection
latency. The lower-latency path becomes active. Ethernet wins when latency is
equal. This comparison repeats approximately every 30 seconds.

The probe only measures TCP connection latency. It does not authenticate to
InfluxDB or perform a complete write.

### Failover and retry

- Ethernet link loss has a 3-second grace period to avoid reacting to brief
  link flaps.
- A connected WiFi standby is promoted immediately when Ethernet fails.
- If WiFi is not already connected, a bounded cold WiFi connection is started.
- Failed WiFi attempts use a cooldown before retrying.
- When Ethernet returns, it is normally preferred again, but the latency
  comparison can select WiFi if WiFi reaches InfluxDB faster.

## LED behavior

### Normal runtime

| Condition | Ethernet LED | WiFi LED |
| --- | --- | --- |
| Ethernet active and database reachable | Solid ON | OFF |
| WiFi active and database reachable | OFF | Solid ON |
| Connected standby interface | 10 seconds ON, 5 seconds OFF | 10 seconds ON, 5 seconds OFF |
| Active interface cannot reach InfluxDB | Quick 200 ms flash every 5 seconds | Quick 200 ms flash every 5 seconds |
| Interface disconnected | OFF | OFF |

The standby timing is a shared 15-second cycle:

- ON for `STANDBY_LED_ON_MS` = 10,000 ms.
- OFF for `STANDBY_LED_OFF_MS` = 5,000 ms.

Database failure indication uses:

- `DB_FAILURE_FLASH_INTERVAL_MS` = 5,000 ms.
- `DB_FAILURE_FLASH_DURATION_MS` = 200 ms.

The database state is marked unavailable when the active client cannot
connect, times out waiting for a response, or receives a response other than
HTTP `204`. A successful `204` write marks it available again.

### WiFiManager portal

Holding the setup button for 3 seconds opens the WiFiManager portal. While
the portal is open, the WiFi LED toggles every 250 ms using the ESP32
`Ticker` library. This is a full 500 ms ON/OFF cycle and is intentionally
different from the normal 10-second/5-second standby pattern.

The ticker is detached when the portal exits. The LED then returns to the
normal connection indication.

## WiFi configuration procedure

1. Power the node.
2. Hold the WiFi setup button on GPIO 14 for at least 3 seconds.
3. Connect a phone or laptop to the access point named by `AP_NAME`.
4. Use the password configured in `AP_PASSWORD`.
5. Open the address shown by WiFiManager, normally `192.168.4.1`.
6. Select the required WiFi network and enter its password.
7. Save the configuration.
8. The node reconnects and resumes normal Ethernet/WiFi selection.

The portal can be opened during boot or during normal runtime. It is
blocking by design while manual configuration is in progress.

## Source-code map

The major functions are organized consistently in each sketch:

| Function | Responsibility |
| --- | --- |
| `getStateName()` | Convert numeric state values to serial labels |
| `isLiftModuleConnected()` | Module supervision hook; currently placeholder |
| `getLiftState()` | Convert lift input and module state to 10/5/0 |
| `getTempStatus()` | Convert temperature and sensor health to 10/5/0 |
| `getOverallStatus()` | Calculate combined alarm/normal state |
| `getActiveClient()` | Select the current Ethernet or WiFi client |
| `measureConnectionLatency()` | Measure TCP latency to InfluxDB |
| `selectBestConnection()` | Choose the lower-latency connected interface |
| `postLineProtocol()` | Send an authenticated HTTP line-protocol write |
| `readTemperatureC()` | Read and validate the DS18B20 |
| `sendLiftEvent()` | Send a changed lift field |
| `sendHeartbeat()` | Send complete lift and temperature status |
| `updateEthernetLed()` | Apply active, standby, and DB-failure indication |
| `updateConnectionMode()` | Run the Ethernet/WiFi state machine |
| `checkWifiSetupButton()` | Detect a runtime portal-button hold |
| `bootCheckWifiButtonHeld()` | Detect a portal-button hold during boot waits |
| `startWifiConfigPortal()` | Run WiFiManager and portal LED indication |
| `bootConnect()` | Perform the bounded boot connection race |
| `setup()` | Initialize hardware, network, sensors, and first heartbeat |
| `loop()` | Maintain connections, process inputs, and send updates |

## Runtime loop

Each loop iteration performs the following:

1. Update Ethernet/WiFi connection state.
2. Update both connectivity LEDs.
3. Check whether the WiFi setup button is held.
4. Read lift inputs.
5. Send an event immediately when either lift changes.
6. Send a complete heartbeat every 3 seconds.
7. Wait 100 ms before the next iteration.

The first complete heartbeat is sent at the end of `setup()` after hardware
and network initialization.

## Arduino IDE setup

Install or select an ESP32 board package and install these libraries:

- `Ethernet`
- `OneWire`
- `DallasTemperature`
- `WiFiManager`
- `Ticker` (provided by the ESP32 Arduino environment in common releases)

Recommended upload checks:

1. Open the intended `.ino` file from its matching folder.
2. Select the correct ESP32 board and serial port.
3. Confirm the user configuration section.
4. Confirm that the target node has a unique IP address and MAC address.
5. Compile before uploading.
6. Open Serial Monitor at `115200 baud`.
7. Verify Ethernet link, WiFi standby/fallback, sensor count, and the first
   heartbeat.

The source files do not include a project-wide automated test suite. Hardware
verification is required after compilation.

## Serial diagnostics

The firmware prints:

- Boot and active connection information.
- DHCP/static-IP result.
- WiFi fallback and hot-standby transitions.
- Ethernet/WiFi TCP latency measurements.
- InfluxDB connection and HTTP errors.
- Sensor count and temperature faults.
- Lift state changes.
- Periodic uptime prefixes for network and event messages.

Use the serial output to determine whether a problem is in the sensor,
physical link, WiFi credentials, InfluxDB endpoint, or database response.

## Troubleshooting

### Ethernet does not connect

- Check W5500 power, reset, SPI wiring, and cable.
- Confirm the switch port is active.
- Check that `STATIC_IP` is unused and on the correct subnet.
- Review DHCP and static-IP messages in Serial Monitor.

### WiFi does not connect

- Open the portal and save the credentials again.
- Confirm the node is within WiFi range.
- Check that the router provides a compatible 2.4 GHz network.
- Wait for the retry cooldown before judging a retry as failed.

### LED indicates database failure

- Confirm `INFLUX_HOST` and `INFLUX_PORT`.
- Confirm that the selected interface can route to the database IP.
- Check InfluxDB availability and firewall rules.
- Verify organization, bucket, and token values.
- HTTP `204` is the success response expected by the firmware.

### Temperature is offline

- Check DS18B20 power, ground, data wiring, and pull-up requirements.
- Confirm the data line is on GPIO 15.
- Check the reported DS18B20 device count.

### Lift remains normal when the wire is disconnected

This is expected with the current implementation. `isLiftModuleConnected()`
is a placeholder that always returns `true`; implement a real supervision
signal before relying on `STATE_OFFLINE` for lift wiring faults.

## Safe maintenance guidelines

- Make shared logic changes in all three sketches.
- Keep node-specific values only in the user configuration section.
- Preserve the unified `node_status` measurement and field names.
- Do not rename `NODE_NAME`, lift names, or database fields without updating
  dashboards, alerts, and queries.
- Do not block the main loop with long network waits.
- Keep connection timeouts bounded.
- Preserve the HTTP `204` success check.
- Do not commit real credentials or tokens to a public repository.
- After changes, run `git diff --check`, compile each sketch, and test:
  Ethernet only, WiFi only, both interfaces, database unavailable, lift
  events, temperature failure, and WiFiManager portal mode.

## Known limitations

- The database token is currently compiled into firmware.
- The lift-module connection detector is not implemented.
- Database reachability status represents the most recent write through the
  active interface; standby interfaces are not independently authenticated
  with a database write.
- Connection latency probes measure TCP connect time rather than complete
  InfluxDB transaction time.
- The WiFiManager portal is intentionally blocking while it is open.
