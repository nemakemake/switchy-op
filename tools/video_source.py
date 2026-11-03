"""Publish a low-delay H.264 camera or test pattern to an existing RTSP server.

Uses the ffmpeg executable, independent of the command gateway and its event loop.
Run `python tools/video_source.py --help` for examples/options.
"""

import argparse
import shlex
import subprocess


def command(args: argparse.Namespace) -> list[str]:
    size = f"{args.width}x{args.height}"
    result = [args.ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin"]
    if args.source == "test":
        result += ["-re", "-f", "lavfi", "-i", f"testsrc2=size={size}:rate={args.fps}"]
    elif args.source == "v4l2":
        result += [
            "-f",
            "v4l2",
            "-framerate",
            str(args.fps),
            "-video_size",
            size,
            "-i",
            args.device,
        ]
    else:
        result += ["-rtsp_transport", "tcp", "-i", args.input_url]
    result += [
        "-map",
        "0:v:0",
        "-an",
        "-vf",
        f"scale={args.width}:{args.height}:force_original_aspect_ratio=decrease:force_divisible_by=2,"
        "setsar=1",
        "-r",
        str(args.fps),
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-profile:v",
        "baseline",
        "-pix_fmt",
        "yuv420p",
        "-bf",
        "0",
        "-g",
        str(args.fps),
        "-keyint_min",
        str(args.fps),
        "-sc_threshold",
        "0",
        "-b:v",
        f"{args.bitrate}k",
        "-maxrate",
        f"{args.bitrate}k",
        "-bufsize",
        f"{args.bitrate // 2}k",
        "-x264-params",
        "repeat-headers=1",
        "-f",
        "rtsp",
        "-rtsp_transport",
        "tcp",
        args.publish,
    ]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg", help="path to ffmpeg with libx264")
    parser.add_argument("--source", choices=("test", "v4l2", "rtsp"), default="test")
    parser.add_argument("--device", default="/dev/video0", help="Linux V4L2 camera")
    parser.add_argument(
        "--input-url", help="existing camera RTSP URL; transcoded to baseline H.264"
    )
    parser.add_argument("--publish", default="rtsp://127.0.0.1:8554/robot")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--fps", type=int, default=25)
    parser.add_argument("--bitrate", type=int, default=1500, help="kilobits per second")
    parser.add_argument(
        "--dry-run", action="store_true", help="print the command without running it"
    )
    args = parser.parse_args()
    if args.source == "rtsp" and not (args.input_url or "").startswith("rtsp://"):
        parser.error("--source rtsp requires --input-url rtsp://...")
    if not args.publish.startswith("rtsp://"):
        parser.error("--publish must start with rtsp://")
    if not (16 <= args.width <= 1280 and 16 <= args.height <= 720):
        parser.error("width/height must fit 16..1280 by 16..720")
    if args.width % 2 or args.height % 2:
        parser.error("width and height must be even for yuv420p")
    if not (1 <= args.fps <= 30 and 100 <= args.bitrate <= 5000):
        parser.error("fps must be 1..30 and bitrate 100..5000 kbit/s")
    cmd = command(args)
    if args.dry_run:
        print(shlex.join(cmd))
        return 0
    try:
        return subprocess.call(cmd)
    except FileNotFoundError:
        parser.exit(1, "ffmpeg not found; install it or set --ffmpeg\n")
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
