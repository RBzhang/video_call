#!/usr/bin/env python3
"""VCL1 UDP test-frame receiver for validating Qt video_call send traffic."""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


MAGIC = 0x56434C31
VERSION = 1
PACKET_TYPE_VIDEO_FRAGMENT = 1
HEADER_FORMAT = "!IBBHIIIIHHHH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 32

MAXIMUM_DATAGRAM_SIZE = 1200
MAXIMUM_PAYLOAD_SIZE = MAXIMUM_DATAGRAM_SIZE - HEADER_SIZE
MAXIMUM_FRAME_SIZE = 4 * 1024 * 1024
MAXIMUM_FRAGMENT_COUNT = 4096

MAXIMUM_PENDING_FRAMES = 16
MAXIMUM_PENDING_PAYLOAD_BYTES = 16 * 1024 * 1024
DEFAULT_TIMEOUT_SECONDS = 0.5

TEST_FRAME_SIZE = 50000
TEST_FRAME_MAGIC = b"VCL_TEST_FRAME_V1"
TEST_FRAME_HEADER_SIZE = len(TEST_FRAME_MAGIC) + 4

FrameKey = Tuple[str, int, int, int]


@dataclass(frozen=True)
class VideoFragment:
    session_id: int
    frame_id: int
    timestamp_ms: int
    frame_size: int
    fragment_index: int
    fragment_count: int
    payload: bytes
    flags: int


@dataclass
class PendingFrame:
    frame_size: int
    fragment_count: int
    timestamp_ms: int
    fragments: List[Optional[bytes]]
    received_count: int
    received_bytes: int
    created_at: float
    last_updated_at: float


@dataclass(frozen=True)
class CompletedFrame:
    key: FrameKey
    timestamp_ms: int
    fragments: int
    payload: bytes
    created_at: float


@dataclass(frozen=True)
class FrameNotice:
    key: FrameKey
    fragment_count: int
    received_count: int
    reason: str
    notice_type: str


@dataclass
class ReassemblyOutcome:
    status: str
    completed_frame: Optional[CompletedFrame] = None
    reason: str = ""
    notices: List[FrameNotice] = field(default_factory=list)


@dataclass
class ReceiverStats:
    received_datagrams: int = 0
    rejected_datagrams: int = 0
    duplicate_fragments: int = 0
    timed_out_frames: int = 0
    conflicting_frames: int = 0
    successfully_reassembled_frames: int = 0
    successfully_validated_test_frames: int = 0
    received_payload_bytes: int = 0


def parse_datagram(datagram: bytes) -> Tuple[Optional[VideoFragment], str]:
    """Parse and strictly validate one VCL1 UDP datagram."""
    datagram_size = len(datagram)
    if datagram_size < HEADER_SIZE:
        return None, "数据报长度小于 32-byte 协议头"
    if datagram_size > MAXIMUM_DATAGRAM_SIZE:
        return None, "数据报大小超过 1200 bytes 协议上限"

    try:
        (magic,
         version,
         packet_type,
         header_size,
         session_id,
         frame_id,
         timestamp_ms,
         frame_size,
         fragment_index,
         fragment_count,
         payload_size,
         flags) = struct.unpack_from(HEADER_FORMAT, datagram, 0)
    except struct.error as error:
        return None, "协议头读取失败: {}".format(error)

    if magic != MAGIC:
        return None, "协议魔数无效"
    if version != VERSION:
        return None, "协议版本不受支持"
    if packet_type != PACKET_TYPE_VIDEO_FRAGMENT:
        return None, "数据包类型无效"
    if header_size != HEADER_SIZE:
        return None, "协议头大小无效"
    if frame_size <= 0:
        return None, "帧大小必须大于零"
    if frame_size > MAXIMUM_FRAME_SIZE:
        return None, "帧大小超过 4 MiB 协议上限"
    if fragment_count <= 0 or fragment_count > MAXIMUM_FRAGMENT_COUNT:
        return None, "分片数量无效"
    if fragment_index >= fragment_count:
        return None, "分片索引无效"
    if payload_size <= 0 or payload_size > MAXIMUM_PAYLOAD_SIZE:
        return None, "分片负载大小无效"

    expected_size = HEADER_SIZE + payload_size
    if datagram_size != expected_size:
        return None, "数据报实际长度与 payloadSize 不一致"
    if frame_size < payload_size:
        return None, "帧大小小于当前分片负载大小"

    payload = datagram[HEADER_SIZE:expected_size]
    if len(payload) != payload_size:
        return None, "分片负载读取失败"

    return VideoFragment(session_id=session_id,
                         frame_id=frame_id,
                         timestamp_ms=timestamp_ms,
                         frame_size=frame_size,
                         fragment_index=fragment_index,
                         fragment_count=fragment_count,
                         payload=payload,
                         flags=flags), ""


class FrameReassembler:
    """Bounded VCL1 reassembly cache matching the Qt receiver's key and limits."""

    def __init__(self, timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS) -> None:
        self.timeout_seconds = timeout_seconds
        self.pending_frames: Dict[FrameKey, PendingFrame] = {}
        self.pending_payload_bytes = 0

    def cleanup_expired(self, current_time: Optional[float] = None) -> List[FrameNotice]:
        now = time.monotonic() if current_time is None else current_time
        notices: List[FrameNotice] = []
        for key, pending in list(self.pending_frames.items()):
            if now - pending.last_updated_at > self.timeout_seconds:
                notices.append(self._remove_pending(
                    key, "未完成帧超时", "timeout"))
        return notices

    def add_fragment(self,
                     fragment: VideoFragment,
                     sender_ip: str,
                     sender_port: int,
                     current_time: Optional[float] = None) -> ReassemblyOutcome:
        now = time.monotonic() if current_time is None else current_time
        notices = self.cleanup_expired(now)
        key = (sender_ip, sender_port, fragment.session_id, fragment.frame_id)
        pending = self.pending_frames.get(key)

        if pending is not None:
            if (pending.frame_size != fragment.frame_size
                    or pending.fragment_count != fragment.fragment_count
                    or pending.timestamp_ms != fragment.timestamp_ms):
                notices.append(self._remove_pending(
                    key, "同一帧的分片元数据不一致", "conflict"))
                return ReassemblyOutcome("rejected", reason="同一帧的分片元数据不一致", notices=notices)

            existing = pending.fragments[fragment.fragment_index]
            if existing is not None:
                if existing == fragment.payload:
                    pending.last_updated_at = now
                    return ReassemblyOutcome("duplicate", notices=notices)
                notices.append(self._remove_pending(
                    key, "重复分片内容冲突", "conflict"))
                return ReassemblyOutcome("rejected", reason="重复分片内容冲突", notices=notices)

            if pending.received_bytes > pending.frame_size - len(fragment.payload):
                notices.append(self._remove_pending(
                    key, "分片累计长度超过声明帧大小", "drop"))
                return ReassemblyOutcome("rejected", reason="分片累计长度超过声明帧大小", notices=notices)

            made_room, eviction_notices = self._make_room(len(fragment.payload), key)
            notices.extend(eviction_notices)
            if not made_room:
                return ReassemblyOutcome("rejected", reason="接收缓存空间不足", notices=notices)
            pending = self.pending_frames.get(key)
            if pending is None:
                return ReassemblyOutcome("rejected", reason="接收缓存已清理该帧", notices=notices)
        else:
            while len(self.pending_frames) >= MAXIMUM_PENDING_FRAMES:
                oldest = self._oldest_key()
                if oldest is None:
                    return ReassemblyOutcome("rejected", reason="接收缓存帧数已达到上限", notices=notices)
                notices.append(self._remove_pending(
                    oldest, "接收缓存达到帧数限制，淘汰最旧未完成帧", "evicted"))

            made_room, eviction_notices = self._make_room(len(fragment.payload))
            notices.extend(eviction_notices)
            if not made_room:
                return ReassemblyOutcome("rejected", reason="接收缓存空间不足", notices=notices)

            pending = PendingFrame(frame_size=fragment.frame_size,
                                   fragment_count=fragment.fragment_count,
                                   timestamp_ms=fragment.timestamp_ms,
                                   fragments=[None] * fragment.fragment_count,
                                   received_count=0,
                                   received_bytes=0,
                                   created_at=now,
                                   last_updated_at=now)
            self.pending_frames[key] = pending

        pending.fragments[fragment.fragment_index] = fragment.payload
        pending.received_count += 1
        pending.received_bytes += len(fragment.payload)
        pending.last_updated_at = now
        self.pending_payload_bytes += len(fragment.payload)

        if pending.received_count != pending.fragment_count:
            return ReassemblyOutcome("accepted", notices=notices)

        if any(piece is None for piece in pending.fragments):
            notices.append(self._remove_pending(key, "完整帧缺少分片", "drop"))
            return ReassemblyOutcome("rejected", reason="完整帧缺少分片", notices=notices)

        payload = b"".join(piece for piece in pending.fragments if piece is not None)
        if pending.received_bytes != pending.frame_size or len(payload) != pending.frame_size:
            notices.append(self._remove_pending(
                key, "重组帧大小与声明大小不一致", "drop"))
            return ReassemblyOutcome("rejected", reason="重组帧大小与声明大小不一致", notices=notices)

        completed = CompletedFrame(key=key,
                                   timestamp_ms=pending.timestamp_ms,
                                   fragments=pending.fragment_count,
                                   payload=payload,
                                   created_at=pending.created_at)
        self._remove_pending(key, "完整帧已重组", "completed")
        return ReassemblyOutcome("completed", completed_frame=completed, notices=notices)

    def _remove_pending(self, key: FrameKey, reason: str, notice_type: str) -> FrameNotice:
        pending = self.pending_frames.pop(key)
        self.pending_payload_bytes = max(0, self.pending_payload_bytes - pending.received_bytes)
        return FrameNotice(key=key,
                           fragment_count=pending.fragment_count,
                           received_count=pending.received_count,
                           reason=reason,
                           notice_type=notice_type)

    def _oldest_key(self, protected_key: Optional[FrameKey] = None) -> Optional[FrameKey]:
        candidates = [key for key in self.pending_frames if key != protected_key]
        if not candidates:
            return None
        return min(candidates,
                   key=lambda key: (self.pending_frames[key].last_updated_at,
                                    self.pending_frames[key].created_at,
                                    key))

    def _make_room(self,
                   required_bytes: int,
                   protected_key: Optional[FrameKey] = None) -> Tuple[bool, List[FrameNotice]]:
        notices: List[FrameNotice] = []
        if required_bytes < 0 or required_bytes > MAXIMUM_PENDING_PAYLOAD_BYTES:
            return False, notices
        while self.pending_payload_bytes > MAXIMUM_PENDING_PAYLOAD_BYTES - required_bytes:
            oldest = self._oldest_key(protected_key)
            if oldest is None:
                return False, notices
            notices.append(self._remove_pending(
                oldest, "接收缓存达到字节限制，淘汰最旧未完成帧", "evicted"))
        return True, notices


def validate_test_frame(frame: bytes) -> Tuple[bool, Optional[int], str]:
    """Validate the exact 50000-byte deterministic frame emitted by MainWindow."""
    if len(frame) != TEST_FRAME_SIZE:
        return False, None, "帧长度不是 50000 bytes"
    if len(frame) < TEST_FRAME_HEADER_SIZE or not frame.startswith(TEST_FRAME_MAGIC):
        return False, None, "测试帧标识无效"

    try:
        sequence = struct.unpack_from("!I", frame, len(TEST_FRAME_MAGIC))[0]
    except struct.error as error:
        return False, None, "测试帧 sequence 读取失败: {}".format(error)

    for index in range(TEST_FRAME_HEADER_SIZE, len(frame)):
        expected = (index * 31 + sequence * 17) & 0xFF
        if frame[index] != expected:
            return False, sequence, "测试帧内容模式不匹配，index={}".format(index)
    return True, sequence, ""


def create_test_frame(sequence: int) -> bytes:
    if sequence < 0 or sequence > 0xFFFFFFFF:
        raise ValueError("sequence 超出 uint32 范围")
    frame = bytearray(TEST_FRAME_MAGIC + struct.pack("!I", sequence))
    frame.extend((index * 31 + sequence * 17) & 0xFF
                 for index in range(len(frame), TEST_FRAME_SIZE))
    return bytes(frame)


def create_datagrams(frame: bytes,
                     session_id: int,
                     frame_id: int,
                     timestamp_ms: int) -> List[bytes]:
    if not frame or len(frame) > MAXIMUM_FRAME_SIZE:
        raise ValueError("帧大小无效")
    fragment_count = (len(frame) + MAXIMUM_PAYLOAD_SIZE - 1) // MAXIMUM_PAYLOAD_SIZE
    if fragment_count <= 0 or fragment_count > MAXIMUM_FRAGMENT_COUNT:
        raise ValueError("分片数量无效")

    datagrams: List[bytes] = []
    for fragment_index in range(fragment_count):
        offset = fragment_index * MAXIMUM_PAYLOAD_SIZE
        payload = frame[offset:offset + MAXIMUM_PAYLOAD_SIZE]
        header = struct.pack(HEADER_FORMAT,
                             MAGIC,
                             VERSION,
                             PACKET_TYPE_VIDEO_FRAGMENT,
                             HEADER_SIZE,
                             session_id,
                             frame_id,
                             timestamp_ms,
                             len(frame),
                             fragment_index,
                             fragment_count,
                             len(payload),
                             0)
        datagram = header + payload
        if len(datagram) > MAXIMUM_DATAGRAM_SIZE:
            raise ValueError("序列化数据报超过协议上限")
        datagrams.append(datagram)
    return datagrams


def print_notice(notice: FrameNotice, now: float) -> None:
    sender_ip, sender_port, session_id, frame_id = notice.key
    if notice.notice_type == "timeout":
        print("[TIMEOUT] source={}:{} session={} frame={} received={}/{}".format(
            sender_ip, sender_port, session_id, frame_id,
            notice.received_count, notice.fragment_count))
        return
    print("[DROP FRAME] source={}:{} session={} frame={} reason={}".format(
        sender_ip, sender_port, session_id, frame_id, notice.reason))


def process_notices(notices: List[FrameNotice], stats: ReceiverStats, now: float) -> None:
    for notice in notices:
        if notice.notice_type == "timeout":
            stats.timed_out_frames += 1
        elif notice.notice_type == "conflict":
            stats.conflicting_frames += 1
        print_notice(notice, now)


def print_summary(stats: ReceiverStats) -> None:
    print("[SUMMARY] datagrams={} rejected={} duplicates={} timeouts={} conflicts={} "
          "reassembled={} valid_test_frames={} payload_bytes={}".format(
              stats.received_datagrams,
              stats.rejected_datagrams,
              stats.duplicate_fragments,
              stats.timed_out_frames,
              stats.conflicting_frames,
              stats.successfully_reassembled_frames,
              stats.successfully_validated_test_frames,
              stats.received_payload_bytes))


def run_receiver(bind_address: str,
                 port: int,
                 timeout_seconds: float,
                 once: bool) -> int:
    stats = ReceiverStats()
    reassembler = FrameReassembler(timeout_seconds)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.bind((bind_address, port))
        sock.settimeout(0.1)
        print("[LISTEN] {}:{} timeout={}s".format(bind_address, port, timeout_seconds))

        while True:
            now = time.monotonic()
            process_notices(reassembler.cleanup_expired(now), stats, now)
            try:
                datagram, source = sock.recvfrom(MAXIMUM_DATAGRAM_SIZE + 1)
            except socket.timeout:
                continue
            except OSError as error:
                print("[SOCKET ERROR] recvfrom failed: {}".format(error), file=sys.stderr)
                return 2

            sender_ip, sender_port = source
            stats.received_datagrams += 1
            fragment, error = parse_datagram(datagram)
            if fragment is None:
                stats.rejected_datagrams += 1
                print("[DROP] source={}:{} reason={}".format(sender_ip, sender_port, error))
                continue

            stats.received_payload_bytes += len(fragment.payload)
            outcome = reassembler.add_fragment(fragment, sender_ip, sender_port, time.monotonic())
            process_notices(outcome.notices, stats, time.monotonic())
            if outcome.status == "duplicate":
                stats.duplicate_fragments += 1
                continue
            if outcome.status == "rejected":
                stats.rejected_datagrams += 1
                if not outcome.notices:
                    print("[DROP FRAME] source={}:{} session={} frame={} reason={}".format(
                        sender_ip, sender_port, fragment.session_id, fragment.frame_id, outcome.reason))
                continue
            if outcome.status != "completed" or outcome.completed_frame is None:
                continue

            completed = outcome.completed_frame
            stats.successfully_reassembled_frames += 1
            valid, sequence, validation_error = validate_test_frame(completed.payload)
            elapsed_ms = (time.monotonic() - completed.created_at) * 1000.0
            if valid:
                stats.successfully_validated_test_frames += 1
                print("[OK] source={}:{} session={} frame={} sequence={} size={} fragments={} "
                      "elapsed_ms={:.1f}".format(
                          completed.key[0], completed.key[1], completed.key[2], completed.key[3],
                          sequence, len(completed.payload), completed.fragments, elapsed_ms))
                if once:
                    return 0
            else:
                print("[INVALID FRAME] source={}:{} session={} frame={} reason={}".format(
                    completed.key[0], completed.key[1], completed.key[2], completed.key[3],
                    validation_error))
    except OSError as error:
        print("[SOCKET ERROR] {}".format(error), file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n[INTERRUPTED]")
        return 0
    finally:
        sock.close()
        print_summary(stats)


def run_self_test() -> int:
    try:
        frame = create_test_frame(3)
        datagrams = create_datagrams(frame, session_id=123456, frame_id=8, timestamp_ms=42)
        if len(datagrams) != 43:
            raise AssertionError("50000-byte 测试帧分片数不是 43")

        parsed_fragments: List[VideoFragment] = []
        for datagram in datagrams:
            fragment, error = parse_datagram(datagram)
            if fragment is None:
                raise AssertionError("合法分片解析失败: {}".format(error))
            parsed_fragments.append(fragment)

        reassembler = FrameReassembler(DEFAULT_TIMEOUT_SECONDS)
        now = time.monotonic()
        first = parsed_fragments[-1]
        first_outcome = reassembler.add_fragment(first, "127.0.0.1", 5000, now)
        if first_outcome.status != "accepted":
            raise AssertionError("首个分片未被接受")
        duplicate_outcome = reassembler.add_fragment(first, "127.0.0.1", 5000, now + 0.001)
        if duplicate_outcome.status != "duplicate":
            raise AssertionError("完全相同重复分片未被忽略")

        completed: Optional[CompletedFrame] = None
        for offset, fragment in enumerate(reversed(parsed_fragments[:-1]), start=2):
            outcome = reassembler.add_fragment(fragment,
                                                "127.0.0.1",
                                                5000,
                                                now + offset * 0.001)
            if outcome.status == "completed":
                completed = outcome.completed_frame
        if completed is None or completed.payload != frame:
            raise AssertionError("倒序分片未能恢复原始测试帧")
        valid, sequence, reason = validate_test_frame(completed.payload)
        if not valid or sequence != 3:
            raise AssertionError("恢复帧校验失败: {}".format(reason))

        bad_magic = bytearray(datagrams[0])
        bad_magic[0] ^= 0x01
        if parse_datagram(bytes(bad_magic))[0] is not None:
            raise AssertionError("错误 magic 未被拒绝")
        if parse_datagram(datagrams[0][:-1])[0] is not None:
            raise AssertionError("截断数据报未被拒绝")

        timeout_reassembler = FrameReassembler(DEFAULT_TIMEOUT_SECONDS)
        timeout_reassembler.add_fragment(parsed_fragments[0], "127.0.0.1", 5000, now)
        expired = timeout_reassembler.cleanup_expired(now + DEFAULT_TIMEOUT_SECONDS + 0.001)
        if len(expired) != 1 or timeout_reassembler.pending_frames:
            raise AssertionError("超时缓存未被清理")

        print("SELF-TEST PASSED")
        return 0
    except (AssertionError, ValueError, struct.error) as error:
        print("SELF-TEST FAILED: {}".format(error), file=sys.stderr)
        return 1


def parse_port(value: str) -> int:
    try:
        port = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("端口必须是整数") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("端口必须在 1～65535 范围内")
    return port


def parse_positive_timeout(value: str) -> float:
    try:
        timeout = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("超时时间必须是数字") from error
    if timeout <= 0:
        raise argparse.ArgumentTypeError("超时时间必须大于 0")
    return timeout


def parse_ipv4_address(value: str) -> str:
    parts = value.split(".")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--bind 必须是 IPv4 地址")
    try:
        octets = [int(part) for part in parts]
    except ValueError as error:
        raise argparse.ArgumentTypeError("--bind 必须是 IPv4 地址") from error
    if any(part == "" or not part.isdigit() or octet < 0 or octet > 255
           for part, octet in zip(parts, octets)):
        raise argparse.ArgumentTypeError("--bind 必须是 IPv4 地址")
    return value


def parse_arguments(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="接收、解析并重组 Qt video_call 发送的 VCL1 UDP 测试帧。")
    parser.add_argument("--bind", type=parse_ipv4_address, default="0.0.0.0",
                        help="本地监听 IPv4 地址（默认：0.0.0.0）")
    parser.add_argument("--port", type=parse_port, default=5000,
                        help="本地 UDP 监听端口，范围 1～65535（默认：5000）")
    parser.add_argument("--timeout", type=parse_positive_timeout,
                        default=DEFAULT_TIMEOUT_SECONDS,
                        help="未完成帧超时秒数（默认：0.5）")
    parser.add_argument("--once", action="store_true",
                        help="成功收到并校验一帧后退出")
    parser.add_argument("--self-test", action="store_true",
                        help="不打开 Socket，只运行协议解析和重组自测试")
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_arguments(argv)
    if args.self_test:
        return run_self_test()
    return run_receiver(args.bind, args.port, args.timeout, args.once)


if __name__ == "__main__":
    sys.exit(main())
