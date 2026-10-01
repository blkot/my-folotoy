# 固件分发网页 —— 给 NAS 用的极简静态站。
#
# 只用 nginx:alpine 托管静态文件:没有后端、没有数据库、不需要 NAS 访问
# GitHub。固件二进制由人手工放进 web/firmwares/(见 README)。
#
# 用 host 网络之外的普通端口映射即可;CH340/ESP32 是接在客户端机器上的,
# 服务器这边不碰串口 —— 刷机完全发生在浏览器与设备之间。

FROM nginx:alpine

# 站点内容
COPY web/ /usr/share/nginx/html/

# 固件二进制单独挂载(见 docker-compose.yml 的 volumes)。
# 放在 web/ 之外是有意的:它是"数据",不该跟着站点文件一起被镜像版本覆盖。
COPY docker/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
