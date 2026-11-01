#!/usr/bin/env python3

import http.server
import json


class MockMoonraker(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()

        # Complete U1 Telemetry Payload
        mock_data = {
            "result": {
                "status": {
                    "temperature_sensor cavity": {"temperature": 44.2},
                    "extruder": {"temperature": 260.0},
                    "heater_bed": {"temperature": 100.0},
                    "toolhead": {
                        "position": [120.0, 110.0, 18.4, 45.2],
                        # "extruder" = T0, "extruder1" = T1, etc.
                        "extruder": "extruder1"
                    },
                    "virtual_sdcard": {"progress": 0.24},
                    "print_stats": {
                        "state": "printing",
                        "print_duration": 91210
                    },
                    "purifier": {
                        "power_detected": True,
                        "power_det_value": 0.0,
                        "mode": 3,
                        "desired_chamber_temp": 50,
                        "critical_chamber_temp": 65,
                        "inner_fan_work_time": 120.0,
                        "inner_fan_rpm": 2400.0,
                        "exhaust_fan": {"speed": 0.0, "delay": 180, "speed_threshold": 0},
                        "inner_fan": {"speed": 0.6, "delay": 180, "speed_threshold": 0}
                    }
                }
            }
        }
        self.wfile.write(json.dumps(mock_data).encode())


print("Mock U1 Moonraker running on port 7125...")
http.server.HTTPServer(('0.0.0.0', 7125), MockMoonraker).serve_forever()
