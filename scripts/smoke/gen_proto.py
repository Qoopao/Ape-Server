"""生成冒烟测试需要的 Python protobuf 绑定。"""
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = pathlib.Path(os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
OUT.mkdir(parents=True, exist_ok=True)
# sdkws 是公共依赖；msg/conversation 供 244 拉历史等单聊用例使用
PROTOS = ['sdkws.proto', 'apeauth.proto', 'groupchat.proto',
          'conversation.proto', 'msg.proto']
subprocess.run(
    ['protoc', f'-I={ROOT / "proto"}', f'--python_out={OUT}', *PROTOS],
    check=True,
)
print(f'python protos -> {OUT}')
