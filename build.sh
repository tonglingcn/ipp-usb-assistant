#!/usr/bin/env bash
# IPP-USB 免驱助手 构建脚本 (Deepin 25, DTK6 + Qt6)
#
# 用法:
#   ./build.sh            # 仅编译
#   ./build.sh --deb      # 编译并打包 deb
#   ./build.sh --clean    # 清理构建目录
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
APP_NAME="ipp-usb-assistant"

case "${1:-}" in
    --clean)
        echo "==> 清理构建目录 ..."
        rm -rf "$BUILD_DIR"
        echo "    已清理: $BUILD_DIR"
        exit 0
        ;;
esac

echo "==> 检查构建依赖 ..."
MISSING=""
for pkg in cmake build-essential pkg-config qt6-base-dev \
           libdtk6core-dev libdtk6gui-dev libdtk6widget-dev \
           libcups2-dev libsane-dev; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ -n "$MISSING" ]; then
    echo "==> 安装缺失的构建依赖 (需要 root 密码) ..."
    sudo apt-get update -y
    # shellcheck disable=SC2086
    sudo apt-get install -y $MISSING
else
    echo "    构建依赖已齐全，跳过安装。"
fi

# 运行时底层服务（外设适配核心）。不安装也能编译，但程序无法正常工作。
if ! dpkg -s ipp-usb >/dev/null 2>&1 || ! dpkg -s sane-airscan >/dev/null 2>&1; then
    echo "==> 安装运行时依赖 (需要 root 密码) ..."
    sudo apt-get install -y \
        ipp-usb \
        cups \
        cups-filters \
        sane \
        sane-airscan \
        avahi-daemon
else
    echo "    运行时依赖已齐全，跳过安装。"
fi

echo "==> 配置与构建 ..."
cd "$PROJECT_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_INSTALL_PREFIX=/usr

# 先单独跑完 AUTOMOC 代码生成（moc_*.cpp），再整体并行编译，
# 避免 clean 后首次构建时 mocs_compilation.cpp.o 抢在最后一个
# moc 文件写出之前开始编译而报 "No such file" 的偶发竞态。
cmake --build "$BUILD_DIR" --target ipp-usb-assistant_autogen -j"$(nproc)"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> 构建完成："
echo "    可执行文件: $BUILD_DIR/$APP_NAME"

# ---- deb 打包 ----
if [ "${1:-}" = "--deb" ]; then
    echo ""
    echo "==> 打包 deb ..."

    # debhelper 需要干净的源码树：构建产物、编辑器备份等不能进包。
    # 这里复制一份到临时目录打包，避免污染工作区。
    PKG_TMP="$(mktemp -d /tmp/ipp-usb-assistant-pkg-XXXXXX)"
    trap 'rm -rf "$PKG_TMP"' EXIT

    tar -cf - --exclude=build --exclude='*.zip' . | (cd "$PKG_TMP" && tar -xf -)

    # 清理上次打包可能残留的中间文件
    rm -f "$PKG_TMP"/debian/files "$PKG_TMP"/debian/*.substvars
    rm -rf "$PKG_TMP"/debian/.debhelper "$PKG_TMP"/debian/ipp-usb-assistant

    (cd "$PKG_TMP" && dpkg-buildpackage -us -uc -b)

    DEB=$(ls -t /tmp/${APP_NAME}_*.deb 2>/dev/null | head -1)
    if [ -n "$DEB" ]; then
        echo "    deb 包: $DEB"
        echo ""
        echo "安装: sudo dpkg -i $DEB"
        echo "卸载: sudo dpkg -r $APP_NAME"
    else
        echo "    [错误] 未找到生成的 deb 包"
        exit 1
    fi
fi

echo ""
echo "运行（建议将当前用户加入 lp/scanner 组以具备设备权限）:"
echo "    $BUILD_DIR/$APP_NAME"
