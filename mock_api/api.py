#!/usr/bin/env python3

import http.server
import argparse
import json


class MockMoonraker(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()

        self.wfile.write(json.dumps(self.server.mock_data).encode())


def build_mock_data(mode, target, chamber, inner_fan, exhaust, bed_target,
                    print_state, sd_file_active):
    return {
            "result": {
                "status": {
                    "temperature_sensor cavity": {"temperature": chamber},
                    "extruder": {
                        "temperature": 260.0
                    },
                    "heater_bed": {
                        "temperature": 100.0,
                        "target": bed_target,
                        "power": 0.0
                    },
                    "toolhead": {
                        "position": [120.0, 110.0, 18.4, 45.2],
                        # "extruder" = T0, "extruder1" = T1, etc.
                        "extruder": "extruder1"
                    },
                    "virtual_sdcard": {
                        "progress": 0.24,
                        "is_active": sd_file_active
                    },
                    "print_stats": {
                        "state": print_state,
                        "print_duration": 91210
                    },
                    "purifier": {
                        "power_detected":True,
                        "power_det_value": 0.7,
                        "mode": mode,
                        "desired_chamber_temp": target,
                        "critical_chamber_temp": 0,
                        "inner_fan_work_time": 7201.6,
                        "exhaust_fan": {
                            "speed": exhaust,
                            "delay": 0,
                            "speed_threshold": 0
                        },
                        "inner_fan": {
                            "speed": inner_fan,
                            "delay": 600,
                            "speed_threshold": 0
                        }
                    },
                }
            }
        }


def main():
    parser = argparse.ArgumentParser(description="Mock U1 Moonraker telemetry")
    parser.add_argument("--mode", type=int, choices=range(4), default=0)
    parser.add_argument("--target", type=float, default=0.0)
    parser.add_argument("--chamber", type=float, default=44.2)
    parser.add_argument("--inner-fan", type=float, default=0.0)
    parser.add_argument("--exhaust", type=float, default=0.0)
    parser.add_argument("--bed-target", type=float, default=0.0)
    parser.add_argument(
        "--print-state",
        choices=("standby", "printing", "paused", "complete", "cancelled", "error"),
        default="printing")
    parser.add_argument(
        "--sd-inactive",
        action="store_true",
        help="report virtual_sdcard.is_active as false")
    args = parser.parse_args()

    server = http.server.HTTPServer(("0.0.0.0", 7125), MockMoonraker)
    server.mock_data = build_mock_data(
        args.mode, args.target, args.chamber, args.inner_fan, args.exhaust,
        args.bed_target, args.print_state, not args.sd_inactive)
    print("Mock U1 Moonraker running on port 7125...")
    server.serve_forever()


if __name__ == "__main__":
    main()
