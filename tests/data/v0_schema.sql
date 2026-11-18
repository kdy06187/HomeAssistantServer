CREATE TABLE devices(
    device_id TEXT PRIMARY KEY,
    display_name TEXT,
    protocol_type INTEGER,
    is_active INTEGER,
    driverTypes TEXT);
CREATE TABLE device_energy_stats(
    device_id TEXT PRIMARY KEY,
    real_total_mwh BIGINT DEFAULT 0,
    last_plug_mwh BIGINT DEFAULT 0);
CREATE TABLE state_history_logs(
    log_id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT,
    total_mwh BIGINT,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP);

INSERT INTO devices VALUES ('SN-MATTER-1', 'Living Plug', 0, 1, '');
INSERT INTO devices VALUES ('ARD_192.168.0.10', 'Desk Lamp', 1, 0, '');
INSERT INTO devices VALUES ('LEGACY_X', 'Other TCP', 1, 1, '');
INSERT INTO device_energy_stats VALUES ('SN-MATTER-1', 5000, 1200);
INSERT INTO state_history_logs (device_id, total_mwh, timestamp) VALUES ('SN-MATTER-1', 100, '2026-01-01 00:00:01');
INSERT INTO state_history_logs (device_id, total_mwh, timestamp) VALUES ('SN-MATTER-1', 200, '2026-01-01 00:00:02');
INSERT INTO state_history_logs (device_id, total_mwh, timestamp) VALUES ('SN-MATTER-1', 300, '2026-01-01 00:00:03');
