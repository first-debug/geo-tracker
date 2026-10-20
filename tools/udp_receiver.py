#!/usr/bin/env python3
"""Принимает UDP-датаграммы sheremetevo-app и печатает их содержимое.

Нужны пакет protobuf (pip install protobuf) и модуль points_pb2,
сгенерированный из proto/points.proto:

    protoc -Iproto --python_out=tools proto/points.proto
    python3 tools/udp_receiver.py --port 6767
"""

import argparse
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import points_pb2  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=6767)
    parser.add_argument("--quiet", action="store_true",
                        help="печатать только сводку по датаграммам, без объектов")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    print(f"listening on {args.host}:{args.port}", file=sys.stderr)

    expected_seq = None
    lost = 0
    while True:
        data, addr = sock.recvfrom(65535)
        batch = points_pb2.Batch()
        try:
            batch.ParseFromString(data)
        except Exception as e:  # noqa: BLE001
            print(f"{addr[0]}: cannot parse {len(data)} bytes: {e}", file=sys.stderr)
            continue

        if expected_seq is not None and batch.sequence != expected_seq:
            if batch.sequence < expected_seq:
                print("sender restarted", file=sys.stderr)
            else:
                lost += batch.sequence - expected_seq
        expected_seq = batch.sequence + 1

        detections = sum(len(f.detections) for f in batch.frames)
        print(f"seq={batch.sequence} sent_at={batch.sent_at:.3f} bytes={len(data)} "
              f"frames={len(batch.frames)} detections={detections} lost_total={lost}")
        if args.quiet:
            continue
        for frame in batch.frames:
            for d in frame.detections:
                print(f"  cam={frame.camera_id} ts={frame.timestamp:.3f} id={d.object_id} "
                      f"{d.name} {d.confidence:.2f} lat={d.lat:.7f} lng={d.lng:.7f} "
                      f"bbox=({d.bbox.x1},{d.bbox.y1},{d.bbox.x2},{d.bbox.y2})")


if __name__ == "__main__":
    main()
