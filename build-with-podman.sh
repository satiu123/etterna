#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="etterna-builder"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${1:-Release}"

echo "==> 检查 Podman 镜像: ${IMAGE_NAME}..."
if ! podman image exists "${IMAGE_NAME}"; then
    echo "==> 镜像不存在，开始构建 ${IMAGE_NAME}..."
    podman build -t "${IMAGE_NAME}" -f "${SCRIPT_DIR}/Containerfile" "${SCRIPT_DIR}"
fi

echo "==> 执行 CMake 配置 (${BUILD_TYPE} 模式)..."
podman run --rm \
    --userns=keep-id \
    -v "${SCRIPT_DIR}:/workspace:Z" \
    -w /workspace \
    "${IMAGE_NAME}" \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DWITH_CRASHPAD=OFF

echo "==> 开始编译 Etterna (Ninja)..."
podman run --rm \
    --userns=keep-id \
    -v "${SCRIPT_DIR}:/workspace:Z" \
    -w /workspace \
    "${IMAGE_NAME}" \
    ninja -C build

echo "==> 编译完成！可执行文件位于:"
ls -lh "${SCRIPT_DIR}/Etterna"* 2>/dev/null || true
