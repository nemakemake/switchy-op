"""Exercise the optional transport without installing ROS or Gazebo."""

import json
import socket
import time

from switchy_op.gazebo import GazeboRobot


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as bridge:
        bridge.bind(("127.0.0.1", 0))
        bridge.settimeout(1)
        robot = GazeboRobot(bridge.getsockname()[1])
        try:
            robot.drive(1, 1)
            data, peer = bridge.recvfrom(8192)
            assert json.loads(data) == {"vx": 0, "wz": 0}
            bridge.sendto(
                json.dumps(dict(ready=True, x=1, y=2, heading=90, speed=0.3)).encode(), peer
            )
            time.sleep(0.02)
            robot.step(0.02)
            assert robot.state()["bridge_ready"]
            assert robot.state()["speed"] == 0.3
            robot.drive(0.5, 0.2)
            assert json.loads(bridge.recv(8192)) == {"vx": 0.5, "wz": 0.2}
            robot.set_switch("estop", True)
            assert json.loads(bridge.recv(8192)) == {"vx": 0, "wz": 0}
            robot.set_switch("estop", False)
            time.sleep(0.55)
            robot.drive(1, 1)
            assert json.loads(bridge.recv(8192)) == {"vx": 0, "wz": 0}
            assert not robot.state()["bridge_ready"]
        finally:
            robot.close()
    print("Gazebo transport checks passed")


if __name__ == "__main__":
    main()
