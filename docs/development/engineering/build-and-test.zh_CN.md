<p align="right">
  <strong>简体中文</strong> · <a href="build-and-test.md">English</a>
</p>

# 构建与验证（Build & Test）

使用 ESP-IDF 5.5.3。全新机器或缺少工具链时，先按
[环境引导](environment-setup.zh_CN.md)完成安装。

> 固件交付构建优先运行 `tools/build-firmware.sh`:它产出唯一发布单文件
> `build/pass-radar_<版本>.bin`(可引导本体 + 44B MPUPV2 指纹尾段,与
> meta-pass 同一契约)并校验保护布局。这个文件从 `0x0` 原样烧入——设备实测、
> 安装页、升级通道用的都是同一个文件。`idf.py build` 和
> `idf.py flash` 只作为增量开发命令，不作为交付方式。

```bash
source <ESP-IDF-v5.5.3-路径>/export.sh
idf.py --version             # 必须输出 ESP-IDF v5.5.3
./tools/validate.sh --firmware # 优先：编译并验证 0x0 合并固件
idf.py set-target esp32c3     # 配置目标芯片（fresh checkout 后/换 target 后运行）
idf.py build                  # 可选：增量 app 编译
idf.py flash monitor          # 可选：增量 app 烧录
idf.py fullclean              # 只清空过期生成状态（勿用于清理用户源码改动）
```

`idf.py fullclean` 不能让已有 `sdkconfig` 完整同步变更后的 defaults。需要重建
target 或已跟踪 defaults 时，先保留有意的本地设置，再运行
`idf.py set-target esp32c3`。

仓库提交 `dependencies.lock` 以固定 ESP-IDF Managed Components 的解析结果。修改 `idf_component.yml` 后必须使用 ESP-IDF 5.5.3 重新生成锁文件、review 版本变化并与 manifest 一起提交；普通构建不应产生未提交的锁文件差异。

固件门禁（`tools/validate.sh --firmware`）使用全新的临时构建目录，并从仓库 `sdkconfig.defaults` 生成隔离的 `sdkconfig`。它不会读取或覆盖开发者根目录的 `sdkconfig`，只把验证通过的合并镜像复制到 `build/pass-radar-full.bin`。门禁同时强制检查[受保护的 Flash 布局](protected-flash-layout.zh_CN.md)：受保护的 `cardid` 地址、应用大小、分区表 MD5 以及单机身份数据不入包。

当前基线含一个可独立运行的纯逻辑测试：

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_ui_pixel_math.c main/ui_pixel_math.c \
  -o /tmp/test_ui_pixel_math
/tmp/test_ui_pixel_math
```

统一验证入口：

```bash
# 先选择现有的 uv 创建的 ESP-IDF Python 环境，再激活工具链。
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.10_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
source <ESP-IDF-v5.5.3-路径>/export.sh
./tools/validate.sh --static    # 仓库一致性、workflow、文档链接、敏感信息、host tests
./tools/validate.sh --firmware  # ESP-IDF build、merge-bin、偏移与保护布局校验
./tools/validate.sh             # 完整验证，要求 ESP-IDF v5.5.3
```

`tools/validate.sh` 要求使用 uv 创建的 Python 虚拟环境，并会打印实际选中的解释器。
如果该环境不在 `PATH` 中，可设置 `PASS_RADAR_PYTHON` 指定 Python 可执行文件；
如果脚本无法确认解释器由 uv 管理，会在测试开始前失败，而不会静默回退到系统
`python3`。CI 与本地使用同一脚本，因此 CI 也应提供等价的 uv 环境；若两者行为不同，
应先修复脚本或环境，而不是维护两份命令。

涉及物理外设的改动必须在真机运行硬件指南验收清单，并把“编译通过”与“硬件验证通过”分开记录。

社区不能上传中间产物（`build/pass-radar.bin`、`build/pass-radar-full.bin`）。
唯一发布文件是 `tools/build-firmware.sh` 产出的 `build/pass-radar_<版本>.bin`
——一种固件兼顾所有场景，与 meta-pass 保持一致。
