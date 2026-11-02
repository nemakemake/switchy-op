#!/usr/bin/env python3
"""Run with ROS 2's system Python, independently of switchy-op's Python 3.14."""

import argparse
import json
import math
import socket
import time

import rclpy
from geometry_msgs.msg import Twist
from nav_msgs.msg import Odometry
from rclpy.qos import qos_profile_sensor_data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=7447)
    parser.add_argument("--cmd-topic", default="/cmd_vel")
    parser.add_argument("--odom-topic", default="/odom")
    args = parser.parse_args()
    rclpy.init(args=[])
    node = rclpy.create_node("switchy_op_bridge")
    publisher = node.create_publisher(Twist, args.cmd_topic, 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.setblocking(False)
    telemetry = {}
    odom_at = 0.0
    command_at = 0.0
    command = (0.0, 0.0)
    peer = None

    def odometry(msg):
        nonlocal odom_at, telemetry
        p, q = msg.pose.pose.position, msg.pose.pose.orientation
        v = msg.twist.twist.linear
        telemetry = dict(
            x=p.x,
            y=p.y,
            speed=math.hypot(v.x, v.y),
            heading=math.degrees(
                math.atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z))
            )
            % 360,
        )
        odom_at = time.monotonic()

    subscription = node.create_subscription(
        Odometry, args.odom_topic, odometry, qos_profile_sensor_data
    )
    try:
        while rclpy.ok():
            rclpy.spin_once(node, timeout_sec=0.01)
            for _ in range(100):
                try:
                    data, address = sock.recvfrom(2048)
                except BlockingIOError:
                    break
                try:
                    packet = json.loads(data)
                    vx, wz = float(packet["vx"]), float(packet["wz"])
                    if not math.isfinite(vx) or not math.isfinite(wz):
                        continue
                except ValueError, TypeError, KeyError:
                    continue
                peer, command, command_at = address, (vx, wz), time.monotonic()
            now = time.monotonic()
            ready = now - odom_at < 0.5
            msg = Twist()
            if ready and now - command_at < 0.4:
                msg.linear.x, msg.angular.z = command
            publisher.publish(msg)
            if peer:
                sock.sendto(json.dumps(dict(telemetry, ready=ready)).encode(), peer)
    except KeyboardInterrupt:
        pass
    finally:
        publisher.publish(Twist())
        sock.close()
        node.destroy_subscription(subscription)
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
