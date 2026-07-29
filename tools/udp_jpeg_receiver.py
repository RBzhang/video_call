#!/usr/bin/env python3
"""Display JPEG frames sent by video_call over the VCL1 UDP protocol."""

from __future__ import annotations

import argparse
import socket
import sys
import time
from dataclasses import dataclass
from typing import List, Optional, Tuple

from udp_test_receiver import (
    DEFAULT_TIMEOUT_SECONDS,
    MAXIMUM_DATAGRAM_SIZE,
    FrameReassembler,
    ReceiverStats,
    parse_datagram,
    parse_ipv4_address,
    parse_port,
    parse_positive_timeout,
    process_notices,
)


WINDOW_TITLE = "VCL1 UDP JPEG Receiver"


@dataclass
class JpegReceiverStatistics:
    completed_frames: int = 0
    decoded_frames: int = 0
    decode_failures: int = 0
    interval_decoded_frames: int = 0
    interval_payload_bytes: int = 0
    interval_jpeg_bytes: int = 0

    def record_decoded_frame(self, jpeg_size: int) -> None:
        self.decoded_frames += 1
        self.interval_decoded_frames += 1
        self.interval_payload_bytes += jpeg_size
        self.interval_jpeg_bytes += jpeg_size

    def reset_interval(self) -> None:
        self.interval_decoded_frames = 0
        self.interval_payload_bytes = 0
        self.interval_jpeg_bytes = 0


def load_opencv() -> Tuple[Optional[object], Optional[object]]:
    try:
        import cv2  # type: ignore
        import numpy  # type: ignore
    except ImportError:
        print("缺少 OpenCV Python，请运行：", file=sys.stderr)
        print("python -m pip install opencv-python", file=sys.stderr)
        return None, None
    return cv2, numpy


def print_statistics(jpeg_stats: JpegReceiverStatistics,
                     receiver_stats: ReceiverStats,
                     elapsed_seconds: float) -> None:
    decoded_frames = jpeg_stats.interval_decoded_frames
    frames_per_second = decoded_frames / elapsed_seconds if elapsed_seconds > 0 else 0.0
    bitrate_mbps = (jpeg_stats.interval_payload_bytes * 8.0
                    / elapsed_seconds / 1000000.0) if elapsed_seconds > 0 else 0.0
    average_jpeg_kb = (jpeg_stats.interval_jpeg_bytes / decoded_frames / 1024.0
                       if decoded_frames else 0.0)
    print("[STATS] fps={:.1f} bitrate_mbps={:.2f} avg_jpeg_kb={:.1f} "
          "completed={} decode_failures={} timeouts={} rejected={}".format(
              frames_per_second,
              bitrate_mbps,
              average_jpeg_kb,
              jpeg_stats.completed_frames,
              jpeg_stats.decode_failures,
              receiver_stats.timed_out_frames,
              receiver_stats.rejected_datagrams))
    jpeg_stats.reset_interval()


def save_first_frame(path: str, encoded: bytes) -> None:
    try:
        with open(path, "wb") as output_file:
            output_file.write(encoded)
        print("[SAVED] first JPEG written to {}".format(path))
    except OSError as error:
        print("[SAVE ERROR] cannot write {}: {}".format(path, error), file=sys.stderr)


def should_quit_display(cv2: object) -> bool:
    key = cv2.waitKey(1) & 0xff
    return key == ord("q") or key == 27


def run_receiver(args: argparse.Namespace, cv2: object, numpy: object) -> int:
    receiver_stats = ReceiverStats()
    jpeg_stats = JpegReceiverStatistics()
    reassembler = FrameReassembler(args.timeout)
    display_enabled = not args.no_display
    save_attempted = False
    printed_first_frame = False
    last_statistics_at = time.monotonic()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.bind((args.bind, args.port))
        sock.settimeout(0.05)
        print("[LISTEN] {}:{} timeout={}s display={}".format(
            args.bind, args.port, args.timeout, display_enabled))

        while True:
            now = time.monotonic()
            process_notices(reassembler.cleanup_expired(now), receiver_stats, now)

            if display_enabled and should_quit_display(cv2):
                return 0

            if now - last_statistics_at >= 1.0:
                print_statistics(jpeg_stats, receiver_stats, now - last_statistics_at)
                last_statistics_at = now

            try:
                datagram, source = sock.recvfrom(MAXIMUM_DATAGRAM_SIZE + 1)
            except socket.timeout:
                continue
            except OSError as error:
                print("[SOCKET ERROR] recvfrom failed: {}".format(error), file=sys.stderr)
                return 2

            sender_ip, sender_port = source
            receiver_stats.received_datagrams += 1
            fragment, error = parse_datagram(datagram)
            if fragment is None:
                receiver_stats.rejected_datagrams += 1
                print("[DROP] source={}:{} reason={}".format(sender_ip, sender_port, error))
                continue

            receiver_stats.received_payload_bytes += len(fragment.payload)
            outcome = reassembler.add_fragment(fragment,
                                                sender_ip,
                                                sender_port,
                                                time.monotonic())
            process_notices(outcome.notices, receiver_stats, time.monotonic())
            if outcome.status == "duplicate":
                receiver_stats.duplicate_fragments += 1
                continue
            if outcome.status == "rejected":
                receiver_stats.rejected_datagrams += 1
                if not outcome.notices:
                    print("[DROP FRAME] source={}:{} session={} frame={} reason={}".format(
                        sender_ip, sender_port, fragment.session_id, fragment.frame_id, outcome.reason))
                continue
            if outcome.status != "completed" or outcome.completed_frame is None:
                continue

            completed = outcome.completed_frame
            receiver_stats.successfully_reassembled_frames += 1
            jpeg_stats.completed_frames += 1
            encoded = completed.payload
            array = numpy.frombuffer(encoded, dtype=numpy.uint8)
            image = cv2.imdecode(array, cv2.IMREAD_COLOR)
            if image is None or image.size == 0:
                jpeg_stats.decode_failures += 1
                print("[INVALID JPEG] source={}:{} session={} frame={} size={}".format(
                    completed.key[0], completed.key[1], completed.key[2], completed.key[3],
                    len(encoded)))
                continue

            height, width = image.shape[:2]
            channels = image.shape[2] if len(image.shape) == 3 else 1
            jpeg_stats.record_decoded_frame(len(encoded))
            if not printed_first_frame:
                printed_first_frame = True
                print("[JPEG] source={}:{} session={} frame={} size={} resolution={}x{} "
                      "channels={} fragments={}".format(
                          completed.key[0], completed.key[1], completed.key[2], completed.key[3],
                          len(encoded), width, height, channels, completed.fragments))

            if args.save_first and not save_attempted:
                save_attempted = True
                save_first_frame(args.save_first, encoded)

            if display_enabled:
                cv2.imshow(WINDOW_TITLE, image)
            if args.once:
                return 0
    except OSError as error:
        print("[SOCKET ERROR] {}".format(error), file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n[INTERRUPTED]")
        return 0
    finally:
        sock.close()
        if display_enabled:
            cv2.destroyAllWindows()
        print("[SUMMARY] datagrams={} rejected={} duplicates={} timeouts={} "
              "completed={} decoded={} decode_failures={} payload_bytes={}".format(
                  receiver_stats.received_datagrams,
                  receiver_stats.rejected_datagrams,
                  receiver_stats.duplicate_fragments,
                  receiver_stats.timed_out_frames,
                  jpeg_stats.completed_frames,
                  jpeg_stats.decoded_frames,
                  jpeg_stats.decode_failures,
                  receiver_stats.received_payload_bytes))


def run_self_test(cv2: object, numpy: object) -> int:
    try:
        y_coordinates, x_coordinates = numpy.indices((480, 640), dtype=numpy.uint16)
        image = numpy.empty((480, 640, 3), dtype=numpy.uint8)
        image[:, :, 0] = (x_coordinates * 3 + y_coordinates) & 0xff
        image[:, :, 1] = (x_coordinates + y_coordinates * 5) & 0xff
        image[:, :, 2] = (x_coordinates * 7 + y_coordinates * 11) & 0xff

        success, encoded = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, 60])
        if not success or encoded is None or encoded.size == 0:
            raise AssertionError("JPEG 编码失败")
        decoded = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
        if decoded is None or decoded.size == 0:
            raise AssertionError("JPEG 解码失败")
        if decoded.shape[0] != 480 or decoded.shape[1] != 640:
            raise AssertionError("JPEG 解码尺寸不是 640x480")

        stats = JpegReceiverStatistics()
        stats.record_decoded_frame(int(encoded.size))
        if stats.decoded_frames != 1 or stats.interval_payload_bytes <= 0:
            raise AssertionError("统计记录失败")

        print("JPEG RECEIVER SELF-TEST PASSED")
        return 0
    except (AssertionError, ValueError) as error:
        print("JPEG RECEIVER SELF-TEST FAILED: {}".format(error), file=sys.stderr)
        return 1


def parse_arguments(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="接收、重组、解码并显示 Qt video_call 发送的 VCL1 JPEG 视频帧。")
    parser.add_argument("--bind", type=parse_ipv4_address, default="0.0.0.0",
                        help="本地监听 IPv4 地址（默认：0.0.0.0）")
    parser.add_argument("--port", type=parse_port, default=5000,
                        help="本地 UDP 监听端口，范围 1～65535（默认：5000）")
    parser.add_argument("--timeout", type=parse_positive_timeout,
                        default=DEFAULT_TIMEOUT_SECONDS,
                        help="未完成帧超时秒数（默认：0.5）")
    parser.add_argument("--once", action="store_true",
                        help="第一帧 JPEG 成功解码后退出")
    parser.add_argument("--no-display", action="store_true",
                        help="不创建 OpenCV 显示窗口")
    parser.add_argument("--save-first", metavar="PATH",
                        help="将第一张成功解码的 JPEG 原始编码数据保存到 PATH")
    parser.add_argument("--self-test", action="store_true",
                        help="不打开 Socket 或显示窗口，只运行 JPEG 编解码自测试")
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_arguments(argv)
    cv2, numpy = load_opencv()
    if cv2 is None or numpy is None:
        return 2
    if args.self_test:
        return run_self_test(cv2, numpy)
    return run_receiver(args, cv2, numpy)


if __name__ == "__main__":
    sys.exit(main())
