# TempShare 局域网 HTTP 接口

所有请求发往桌面端显示的地址。服务校验 Host，拒绝公网来源，只接受私有 IPv4 地址或本机回环连接。同一私有网络中的设备如果使用不同子网，路由器必须允许彼此访问。浏览器需要保留 `ts_<端口>` 会话 Cookie。写请求需要带 `X-TempShare: 1`。正文长度由 `Content-Length` 指定。

## 配对和状态

| 请求 | 参数与结果 |
| --- | --- |
| `GET /api/health` | 返回服务名称，用于确认当前地址可访问。 |
| `POST /api/pair` | `X-Invitation` 为六位短码或二维码中的邀请令牌。可使用编码后的 `X-Device-Name`。成功返回 201、验证码和会话 Cookie，桌面批准后获得访问权限。 |
| `GET /api/pair` | 返回 `state`、权限、列表版本和 `castReady`。401 表示会话不可用。 |
| `DELETE /api/pair` | 撤销当前请求或离开当前设备，成功返回 204。共享清单和消息保留，未完成上传和投屏停止；这不等同于桌面「结束会话」。 |
| `POST /api/pair/extend` | 已配对设备显式延长本会话30分钟，成功返回204。 |
| `POST /api/pair/trusted` | HTTPS 下使用此前保存的 `ts_<端口>_trusted` Cookie 快速配对，兼容已有的旧版 Cookie。 |

`filesRevision`、`offersRevision` 和 `textRevision` 是资源变化提示。版本变化后重新读取对应列表。版本值由当前服务进程维护。

0.4.0通过同一公开端口提供额外会话路径 `/s/2/` 至 `/s/4/`。以上所有API相对于该会话前缀调用，静态资源共用根路径。新浏览器进入繁忙根入口时收到空闲会话路径的跳转；已持有当前Cookie的浏览器不会被移走。HTTPS信任使用设备级 `ts_trusted` Cookie和公共信任记录，同时兼容旧的端口Cookie。

配对状态增加 `castAllowed`，表示屏幕共享/观看授权；网页显示 `remainingSeconds` 并提供延长入口。普通配对不允许远程使用 `/cast` 或创建本机采集密钥。

## 原生桌面连接（0.4.0）

- `POST /api/desktop/pair` 提交待本机审批的桌面请求。包含 `X-Desktop-Key`（发起方回调访问令牌）、`X-Desktop-ID`、`X-Desktop-Port`、`X-Desktop-Route`、`X-Desktop-Secure`、编码后的 `X-Device-Name`，以及可选的 `X-Invitation`。没有邀请码的局域网发现请求只能创建待审批项，不能自动获得权限。繁忙入口可返回307和受限的 `/s/N` 路径。
- `GET /api/desktop/state` 使用 `X-Desktop-Key` 授权轮询。响应为有界长度前缀字段：版本、设备名、批准状态、验证码、download/upload/text/cast权限、屏幕就绪、清空标记、消息数量及每条sequence/time/text/attachment、文件数量及每项id/name/size/folder。仅导出本机发送的消息，避免双向回声。单响应不超过4MiB，单字段不超过64KiB；接收端验证计数、令牌及文件名称。
- `GET /api/desktop/offer/<id>` 返回本连接上传请求的状态与已保存偏移。`POST /api/offers`额外返回 `X-Offer-ID`，用于原生发送流程。
- 双端保留各自的本机采集授权；观看远端通过本机 `/peer` 页面和 `/peer/api/*` 中转。浏览器需先以URL fragment里的短期本机授权调用 `POST /peer/authorize`，随后使用HttpOnly Cookie。原生远端令牌不交给浏览器。此入口仅允许回环访问，不能作为任意公网代理；上传正文最多4MiB，下载按流转发。
- 收到请求的一端必须显式本机批准。远端自称已批准不能绕过此限制；发起方在对方批准后才更新自己对应的会话状态。断线状态和身份批准分开，后台线程有界超时并在销毁前回收。

## 文件

- `GET /api/files` 返回已分享文件的名称、大小和 ID。使用 `GET /api/download/<id>` 下载，支持 HTTP Range。
- `GET /api/files?folders=1` 同时返回可浏览的文件夹。目录条目带有 `folder: true`。
- `GET /api/folders/<root>/entries/<directory>` 返回指定目录的直属子项。浏览顶层时，`directory` 填 `root`。无法下载的文件带有 `unavailable: true`。目录清单在桌面共享时生成。
- 传入 `offset` 以每页至多 80 项读取目录，并从 `X-Folder-Count` 获取筛选后的数量。可指定 `q` 搜索文件名和 `sort=size` 按大小排序。
- `GET /api/folders/<root>/download/<id>` 下载该共享目录中的单个文件，支持 HTTP Range。两条目录接口都需要配对批准及下载权限，移除根目录后会同时撤销所有子项的访问。
- `GET /api/folders/<root>/zip/<directory>` 归档当前目录。用重复的 `file=<id>` 参数指定至多 80 个当前目录的直属条目。归档使用共享清单，并检查源文件版本和下载权限。
- 下载接口接受 `HEAD` 检查可用性。列表中的 `preview` 指明支持的媒体类型，使用同一下载链接并加上 `preview=1` 获取可预览内容。
- `POST /api/offers` 带 URL 编码的 `X-File-Name` 和字节数 `X-File-Size` 创建上传请求。桌面批准后，状态从 `pending` 进入 `approved`。用 `GET /api/offers` 读取状态，`DELETE /api/offers/<id>` 取消请求。
- 可选的 `X-Upload-Batch` 为同次文件选择共享的 32 位十六进制标识。`X-Upload-Directory` 为 URL 编码的相对父目录。每一级目录由服务端验证。
- `PUT /api/upload/<id>` 写入字节。分块请求带 `X-Upload-Offset`，单块最多 4 MiB。服务器校验偏移并在成功时返回已保存的字节数 `transferred`。最终块发布文件，状态进入 `complete`。同一请求中断后，先读取最新状态与偏移再重试。旧客户端在主会话根路径可用无偏移的 PUT 一次上传整份文件；新增 `/s/N` 与本机 `/peer` 转发入口限制单请求 4 MiB，上传须使用分块。
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
- 观看端可在刷新租约时带上 `X-Cast-Frames: 1` 请求兼容播放，并通过返回的 `frames` 字段确认。控制台使用本机授权和当前 `X-Cast-Revision` 向 `POST /api/cast/frame` 提交不超过 384 KiB 的 JPEG。已配对的观看端携带原租约标识从 `GET /api/cast/frame` 获取单帧，可用 `X-Cast-After` 传入上次响应的 `X-Cast-Sequence`。无更新时返回 204。
- 兼容播放可以加 `X-Cast-Wait: 1` 等待下一帧，最多 800 毫秒。每个连接同时允许一个等待请求。

[局域网 HTTPS 与设备验收](lan-https.md)
