#!/usr/bin/env python3
"""Generates grafana/sump-dashboard.json (InfluxDB 2.x, Flux).

Edit the queries or layout here and re-run: python3 tools/make_dashboard.py
"""
import json
import os

DS = {"type": "influxdb", "uid": "${DS_INFLUX}"}
DEV = 'r.device == "${device}"'
TZ = 'import "timezone"\noption location = timezone.location(name: "America/New_York")\n'

_next_id = [0]


def pid():
    _next_id[0] += 1
    return _next_id[0]


def tgt(ref, query):
    return {"refId": ref, "datasource": DS, "query": query}


def rng():
    return 'from(bucket: "${bucket}")\n  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'


def series(measurement, field, name, extra=""):
    """One named series from a numeric field, over the dashboard range."""
    return (rng() +
            f'  |> filter(fn: (r) => r._measurement == "{measurement}" and r._field == "{field}" and {DEV})\n'
            f'{extra}'
            '  |> keep(columns: ["_time", "_value"])\n'
            f'  |> rename(columns: {{_value: "{name}"}})')


def state_series(measurement, field, name):
    """A 0/1 state over the range, carrying in the last value from before it."""
    flt = f'r._measurement == "{measurement}" and r._field == "{field}" and {DEV}'
    return (
        f'prior = from(bucket: "${{bucket}}")\n  |> range(start: -90d, stop: v.timeRangeStart)\n'
        f'  |> filter(fn: (r) => {flt})\n  |> last()\n'
        '  |> map(fn: (r) => ({r with _time: v.timeRangeStart}))\n'
        f'data = {rng()}  |> filter(fn: (r) => {flt})\n'
        'union(tables: [prior, data])\n'
        '  |> keep(columns: ["_time", "_value"])\n  |> group()\n  |> sort(columns: ["_time"])\n'
        f'  |> rename(columns: {{_value: "{name}"}})')


def hline(var, name):
    """A flat line at a dashboard variable's value, e.g. the high-water level."""
    return ('import "array"\narray.from(rows: [\n'
            f'  {{_time: v.timeRangeStart, _value: float(v: "${{{var}}}")}},\n'
            f'  {{_time: v.timeRangeStop, _value: float(v: "${{{var}}}")}},\n'
            f'])\n  |> rename(columns: {{_value: "{name}"}})')


def last_value(measurement, field, lookback="-1h"):
    return (f'from(bucket: "${{bucket}}")\n  |> range(start: {lookback})\n'
            f'  |> filter(fn: (r) => r._measurement == "{measurement}" and r._field == "{field}" and {DEV})\n'
            '  |> last()\n  |> keep(columns: ["_time", "_value"])')


def by_name(name, props):
    return {"matcher": {"id": "byName", "options": name},
            "properties": [{"id": k, "value": v} for k, v in props.items()]}


def panel(ptype, title, x, y, w, h, targets, defaults=None, overrides=None, options=None, **extra):
    p = {"id": pid(), "type": ptype, "title": title, "datasource": DS,
         "gridPos": {"x": x, "y": y, "w": w, "h": h}, "targets": targets,
         "fieldConfig": {"defaults": defaults or {}, "overrides": overrides or []},
         "options": options or {}}
    p.update(extra)
    return p


ON_OFF = [{"type": "value", "options": {
    "1": {"text": "ON", "color": "green", "index": 0},
    "0": {"text": "OFF", "color": "#6e6e6e", "index": 1}}}]
HIGH_OK = [{"type": "value", "options": {
    "1": {"text": "HIGH WATER", "color": "red", "index": 0},
    "0": {"text": "OK", "color": "transparent", "index": 1}}}]


def stat(title, x, query, defaults, options=None):
    opts = {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
            "colorMode": "value", "graphMode": "none", "textMode": "value", "justifyMode": "center"}
    opts.update(options or {})
    return panel("stat", title, x, 0, 4, 4, [tgt("A", query)], defaults, options=opts)


panels = []

# ---- Row 1: current values --------------------------------------------------
panels.append(stat("Pump", 0, last_value("sump_event", "running", "-90d"),
                   {"mappings": ON_OFF, "color": {"mode": "thresholds"},
                    "thresholds": {"mode": "absolute", "steps": [
                        {"color": "#6e6e6e", "value": None}, {"color": "green", "value": 1}]}},
                   {"colorMode": "background"}))
panels.append(stat("Water depth", 4, last_value("sump", "level_cm"),
                   {"unit": "suffix: cm", "decimals": 1, "color": {"mode": "thresholds"},
                    "thresholds": {"mode": "absolute", "steps": [
                        {"color": "green", "value": None}, {"color": "orange", "value": 35},
                        {"color": "red", "value": 45}]},
                    "noValue": "no reading"}))
panels.append(stat("Air temperature", 8, last_value("sump", "temp_c"),
                   {"unit": "celsius", "decimals": 1, "color": {"mode": "fixed", "fixedColor": "text"},
                    "noValue": "no reading"}))
panels.append(stat("Humidity", 12, last_value("sump", "humidity"),
                   {"unit": "percent", "decimals": 0, "color": {"mode": "fixed", "fixedColor": "text"},
                    "noValue": "no reading"}))
panels.append(stat("Pump cycles (range)", 16,
                   rng() + '  |> filter(fn: (r) => r._measurement == "sump_event" and r._field == "seconds" and '
                   + DEV + ')\n  |> count()\n  |> keep(columns: ["_value"])',
                   {"decimals": 0, "color": {"mode": "fixed", "fixedColor": "text"}, "noValue": "0"}))
panels.append(stat("Last data", 20,
                   'from(bucket: "${bucket}")\n  |> range(start: -30d)\n'
                   '  |> filter(fn: (r) => r._measurement =~ /^sump/ and ' + DEV + ')\n'
                   '  |> keep(columns: ["_time"])\n  |> group()\n  |> sort(columns: ["_time"])\n  |> tail(n: 1)\n'
                   '  |> map(fn: (r) => ({_time: r._time, _value: int(v: r._time) / 1000000}))',
                   {"unit": "dateTimeFromNow", "color": {"mode": "fixed", "fixedColor": "text"},
                    "noValue": "never"}))

# ---- Row 2: state timeline ---------------------------------------------------
panels.append(panel(
    "state-timeline", "Pump and alarms", 0, 4, 24, 5,
    [tgt("A", state_series("sump_event", "running", "Pump")),
     tgt("B", state_series("sump_alarm", "active", "High water"))],
    {"color": {"mode": "thresholds"},
     "thresholds": {"mode": "absolute", "steps": [{"color": "#6e6e6e", "value": None}]},
     "custom": {"fillOpacity": 80, "lineWidth": 0, "spanNulls": True, "insertNulls": False}},
    [by_name("Pump", {"mappings": ON_OFF}), by_name("High water", {"mappings": HIGH_OK})],
    {"mergeValues": True, "showValue": "auto", "alignValue": "center", "rowHeight": 0.8,
     "legend": {"showLegend": False}, "tooltip": {"mode": "single"}}))

# ---- Row 3: water level --------------------------------------------------------
TS_DEFAULTS = {"custom": {"drawStyle": "line", "lineWidth": 2, "fillOpacity": 10,
                          "pointSize": 4, "showPoints": "never", "spanNulls": 1800000}}


def points(size=7):
    return {"custom.drawStyle": "points", "custom.pointSize": size}


panels.append(panel(
    "timeseries", "Water depth above the pit floor", 0, 9, 24, 9,
    [tgt("A", series("sump", "level_cm", "Water depth")),
     tgt("B", series("sump_cycle", "level_before_cm", "Pump on at")),
     tgt("C", series("sump_cycle", "level_after_cm", "Pump off at")),
     tgt("D", hline("high_water_cm", "High-water alarm"))],
    dict(TS_DEFAULTS, unit="suffix: cm", decimals=1, min=0),
    [by_name("Pump on at", dict(points(), color={"mode": "fixed", "fixedColor": "orange"})),
     by_name("Pump off at", dict(points(), color={"mode": "fixed", "fixedColor": "green"})),
     by_name("High-water alarm", {"color": {"mode": "fixed", "fixedColor": "red"},
                                  "custom.lineStyle": {"fill": "dash", "dash": [10, 10]},
                                  "custom.fillOpacity": 0, "custom.lineWidth": 1})],
    {"legend": {"showLegend": True, "displayMode": "list", "placement": "bottom"},
     "tooltip": {"mode": "multi"}},
    description="Water depth every 10 minutes, plus the level just before (on) and after (off) each "
                "pump run. The on/off points show the pump's real float levels, for setting "
                "HIGH_WATER_CM. The dashed line is the high_water_cm variable (keep it equal to the "
                "firmware's HIGH_WATER_CM)."))

# ---- Row 4: pump behaviour -------------------------------------------------------
panels.append(panel(
    "timeseries", "Run time per pump cycle", 0, 18, 12, 8,
    [tgt("A", series("sump_event", "seconds", "Run time"))],
    dict(TS_DEFAULTS, unit="s", decimals=1, min=0),
    [by_name("Run time", points(6))],
    {"legend": {"showLegend": False}, "tooltip": {"mode": "single"}},
    description="Length of every run. A slow rise can mean a worn pump or a partly blocked discharge."))
panels.append(panel(
    "timeseries", "Level drop per pump cycle", 12, 18, 12, 8,
    [tgt("A", series("sump_cycle", "drop_cm", "Drop")),
     tgt("B", hline("min_drop_cm", "Minimum expected"))],
    dict(TS_DEFAULTS, unit="suffix: cm", decimals=1),
    [by_name("Drop", points(6)),
     by_name("Minimum expected", {"color": {"mode": "fixed", "fixedColor": "red"},
                                  "custom.lineStyle": {"fill": "dash", "dash": [10, 10]},
                                  "custom.fillOpacity": 0, "custom.lineWidth": 1})],
    {"legend": {"showLegend": True, "displayMode": "list", "placement": "bottom"},
     "tooltip": {"mode": "multi"}},
    description="How far each run lowered the water. Below the dashed line triggers the "
                "'pump ran but level didn't drop' push (min_drop_cm should match MIN_DROP_CM)."))


def per_day(fn, field, name, scale=""):
    return (TZ + rng() +
            f'  |> filter(fn: (r) => r._measurement == "sump_event" and r._field == "{field}" and {DEV})\n'
            + scale +
            f'  |> aggregateWindow(every: 1d, fn: {fn}, createEmpty: true, timeSrc: "_start")\n'
            '  |> keep(columns: ["_time", "_value"])\n'
            f'  |> rename(columns: {{_value: "{name}"}})')


BAR = {"xTickLabelRotation": 0, "legend": {"showLegend": False}, "stacking": "none",
       "xField": "_time", "barWidth": 0.8}
panels.append(panel(
    "barchart", "Pump cycles per day", 0, 26, 12, 8,
    [tgt("A", per_day("count", "seconds", "Cycles"))],
    {"decimals": 0, "color": {"mode": "fixed", "fixedColor": "blue"}}, options=BAR))
panels.append(panel(
    "barchart", "Pump run time per day", 12, 26, 12, 8,
    [tgt("A", per_day("sum", "seconds", "Run time",
                      '  |> map(fn: (r) => ({r with _value: r._value / 60.0}))\n'))],
    {"unit": "m", "decimals": 1, "color": {"mode": "fixed", "fixedColor": "blue"}}, options=BAR))

# ---- Row 5: air ----------------------------------------------------------------------
panels.append(panel(
    "timeseries", "Temperature and humidity", 0, 34, 24, 8,
    [tgt("A", series("sump", "temp_c", "Temperature")),
     tgt("B", series("sump", "humidity", "Humidity"))],
    dict(TS_DEFAULTS, decimals=1),
    [by_name("Temperature", {"unit": "celsius", "color": {"mode": "fixed", "fixedColor": "orange"}}),
     by_name("Humidity", {"unit": "percent", "custom.axisPlacement": "right",
                          "color": {"mode": "fixed", "fixedColor": "blue"}})],
    {"legend": {"showLegend": True, "displayMode": "list", "placement": "bottom"},
     "tooltip": {"mode": "multi"}}))

# ---- Row 6: tables ---------------------------------------------------------------------
cycles_query = (
    'nums = ' + rng() +
    '  |> filter(fn: (r) => r._measurement == "sump_cycle" and r._field != "result" and ' + DEV + ')\n'
    '  |> group(columns: ["_measurement"])\n'
    '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> group()\n'
    'res = ' + rng() +
    '  |> filter(fn: (r) => r._measurement == "sump_cycle" and r._field == "result" and ' + DEV + ')\n'
    '  |> keep(columns: ["_time", "_value"])\n  |> rename(columns: {_value: "result"})\n  |> group()\n'
    'join(tables: {n: nums, r: res}, on: ["_time"])\n'
    '  |> keep(columns: ["_time", "seconds", "level_before_cm", "level_after_cm", "drop_cm", "result"])\n'
    '  |> sort(columns: ["_time"], desc: true)\n  |> limit(n: 100)')
panels.append(panel(
    "table", "Recent pump cycles", 0, 42, 14, 9, [tgt("A", cycles_query)],
    {"decimals": 1},
    [by_name("_time", {"displayName": "Stopped at"}),
     by_name("seconds", {"displayName": "Ran", "unit": "s"}),
     by_name("level_before_cm", {"displayName": "On at (cm)"}),
     by_name("level_after_cm", {"displayName": "Off at (cm)"}),
     by_name("drop_cm", {"displayName": "Drop (cm)"}),
     by_name("result", {"displayName": "Check", "mappings": [{"type": "value", "options": {
         "ok": {"text": "ok", "color": "green", "index": 0},
         "no_drop": {"text": "DIDN'T DROP", "color": "red", "index": 1},
         "unverified": {"text": "no reading", "color": "#6e6e6e", "index": 2}}}],
                         "custom.cellOptions": {"type": "color-text"}})],
    {"showHeader": True}))

events_query = (
    'alarms = ' + rng() +
    '  |> filter(fn: (r) => r._measurement == "sump_alarm" and r._field == "active" and ' + DEV + ')\n'
    '  |> group()\n'
    '  |> map(fn: (r) => ({_time: r._time, event: if r._value == 1 then "High water" else "High water cleared"}))\n'
    'boots = ' + rng() +
    '  |> filter(fn: (r) => r._measurement == "sump_boot" and ' + DEV + ')\n'
    '  |> group(columns: ["_measurement"])\n'
    '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
    '  |> group()\n'
    '  |> map(fn: (r) => ({_time: r._time, event: "Boot " + r.version + ": " + r.reason}))\n'
    'union(tables: [alarms, boots])\n  |> group()\n'
    '  |> sort(columns: ["_time"], desc: true)\n  |> limit(n: 100)')
panels.append(panel(
    "table", "Alarms and reboots", 14, 42, 10, 9, [tgt("A", events_query)], {},
    [by_name("_time", {"displayName": "Time", "custom.width": 170}),
     by_name("event", {"displayName": "Event"})],
    {"showHeader": True}))

# ---- Row 7: board health --------------------------------------------------------------
panels.append(panel(
    "timeseries", "Board: Wi-Fi signal and free memory", 0, 51, 24, 7,
    [tgt("A", series("sump", "rssi", "Wi-Fi RSSI")),
     tgt("B", series("sump", "heap", "Free heap"))],
    dict(TS_DEFAULTS, decimals=0),
    [by_name("Wi-Fi RSSI", {"unit": "dBm"}),
     by_name("Free heap", {"unit": "bytes", "custom.axisPlacement": "right"})],
    {"legend": {"showLegend": True, "displayMode": "list", "placement": "bottom"},
     "tooltip": {"mode": "multi"}},
    description="A steady fall in free heap between nightly reboots would point to a leak."))

annotations = {"list": [
    {"builtIn": 1, "datasource": {"type": "grafana", "uid": "-- Grafana --"}, "enable": True,
     "hide": True, "iconColor": "rgba(0, 211, 255, 1)", "name": "Annotations & Alerts",
     "type": "dashboard"},
    {"datasource": DS, "enable": True, "iconColor": "red", "name": "High water",
     "target": {"refId": "Anno", "query":
                rng() + '  |> filter(fn: (r) => r._measurement == "sump_alarm" and r._field == "active" and '
                + DEV + ')\n  |> group()\n  |> map(fn: (r) => ({_time: r._time, text: if r._value == 1 then '
                '"High water" else "High water cleared"}))'}},
    {"datasource": DS, "enable": True, "iconColor": "blue", "name": "Reboots",
     "target": {"refId": "Anno", "query":
                rng() + '  |> filter(fn: (r) => r._measurement == "sump_boot" and ' + DEV + ')\n'
                '  |> group(columns: ["_measurement"])\n'
                '  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")\n'
                '  |> group()\n'
                '  |> map(fn: (r) => ({_time: r._time, text: "Boot " + r.version + ": " + r.reason}))'}},
]}


def textbox(name, label, value, description):
    return {"name": name, "label": label, "type": "textbox", "query": value, "description": description,
            "current": {"text": value, "value": value},
            "options": [{"selected": True, "text": value, "value": value}]}


dashboard = {
    "__inputs": [{"name": "DS_INFLUX", "label": "InfluxDB (Flux)", "type": "datasource",
                  "pluginId": "influxdb", "pluginName": "InfluxDB"}],
    "title": "Sump Master 2000",
    "uid": "sump-master-2000",
    "description": "Sump pump, water level and air readings from the Sump Master 2000 board.",
    "schemaVersion": 39,
    "version": 1,
    "tags": ["sump"],
    "time": {"from": "now-24h", "to": "now"},
    "refresh": "1m",
    "timezone": "browser",
    "graphTooltip": 1,
    "annotations": annotations,
    "templating": {"list": [
        textbox("bucket", "Bucket", "CHANGE_ME", "InfluxDB bucket Telegraf writes to"),
        textbox("device", "Device", "sump", "DEVICE_NAME of the board"),
        textbox("high_water_cm", "High water (cm)", "45", "Keep equal to the firmware's HIGH_WATER_CM"),
        textbox("min_drop_cm", "Min drop (cm)", "5", "Keep equal to the firmware's MIN_DROP_CM"),
    ]},
    "panels": panels,
}

out = os.path.join(os.path.dirname(__file__), "..", "grafana", "sump-dashboard.json")
with open(out, "w") as f:
    json.dump(dashboard, f, indent=2)
    f.write("\n")
print(f"wrote {os.path.normpath(out)} ({len(panels)} panels)")
