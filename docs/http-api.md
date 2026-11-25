# TempShare 局域网 HTTP 接口

所有请求发往桌面端显示的地址。服务校验 Host，拒绝公网来源，只接受私有 IPv4 地址或本机回环连接。同一私有网络中的设备如果使用不同子网，路由器必须允许彼此访问。浏览器需要保留 `ts_<端口>` 会话 Cookie。写请求需要带 `X-TempShare: 1`。正文长度由 `Content-Length` 指定。

## 配对和状态

| 请求 | 参数与结果 |
| --- | --- |
| `GET /api/health` | 返回服务名称，用于确认当前地址可访问。 |
| `POST /api/pair` | `X-Invitation` 为六位短码或二维码中的邀请令牌。可使用编码后的 `X-Device-Name`。成功返回 201、验证码和会话 Cookie，桌面批准后获得访问权限。 |
| `GET /api/pair` | 返回 `state`、权限、列表版本和 `castReady`。401 表示会话不可用。 |
| `DELETE /api/pair` | 撤销当前请求或配对会话，成功返回 204。 |
| `POST /api/pair/trusted` | HTTPS 下使用此前保存的 `ts_trusted` Cookie 快速配对。 |

`filesRevision`、`offersRevision` 和 `textRevision` 是资源变化提示。版本变化后重新读取对应列表。版本值由当前服务进程维护。

## 文件

- `GET /api/files` 返回已分享文件的名称、大小和 ID。使用 `GET /api/download/<id>` 下载，支持 HTTP Range。
- `POST /api/offers` 带 URL 编码的 `X-File-Name` 和字节数 `X-File-Size` 创建上传请求。桌面批准后，状态从 `pending` 进入 `approved`。用 `GET /api/offers` 读取状态，`DELETE /api/offers/<id>` 取消请求。
- `PUT /api/upload/<id>` 写入字节。分块请求带 `X-Upload-Offset`，单块最多 4 MiB。服务器校验偏移并在成功时返回已保存的字节数 `transferred`。最终块发布文件，状态进入 `complete`。同一请求中断后，先读取最新状态与偏移再重试。旧客户端可用无偏移的 PUT 一次上传整份文件。
- 本地会话的总接收量为 4 GiB，单文件最多 1 GiB。服务为控制请求留出工作线程，同时处理最多 4 个文件上传或下载。

失败响应保留 `error` 文本。已细分的失败通过可选 `code` 返回稳定标识。

| HTTP 状态 | `code` | 客户端动作 |
| --- | --- | --- |
| 409 | `file_changed` | 请求桌面重新选择文件。 |
| 409 | `upload_unavailable` | 刷新请求状态。用服务端报告的偏移重试。 |
| 503 | `transfer_busy` | 按 `Retry-After` 等待后重试。 |
| 507 | `disk_space`、`storage_unavailable` | 提示检查目标磁盘空间、目录和权限。 |

## 文本和投递

- `GET /api/text` 获取双向文本记录，`POST /api/text` 发送最长 32 KiB 的 UTF-8 正文。
- 本机投递控制台通过 `X-Cast-Key` 使用 `/api/cast/host`、`/api/cast/ready`、`/api/cast/offer` 和 `/api/cast/stop`。
- 已配对的观看端向 `POST /api/cast/viewer` 发送 `X-Cast-Watch: 1`，并为当前观看操作生成一个 32 位十六进制值，放在 `X-Cast-Viewer` 中。定期刷新；退出时携带同一标识和 `X-Cast-Watch: 0`。活跃租约被其他标签页占用时返回 409。旧客户端可沿用无 `X-Cast-Viewer` 的单观看者流程。
- `POST /api/cast/answer` 带同一观看标识及 `X-Cast-Revision`，正文为 SDP。信令由桌面端提供，媒体经浏览器间的 WebRTC 流传输。

[局域网 HTTPS 与设备验收](lan-https.md)
