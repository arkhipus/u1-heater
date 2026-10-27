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
                    "purifier": {"speed": 0.0, "is_enabled": False}
                }
            }
        }
        self.wfile.write(json.dumps(mock_data).encode())


print("Mock U1 Moonraker running on port 7125...")
http.server.HTTPServer(('0.0.0.0', 7125), MockMoonraker).serve_forever()
