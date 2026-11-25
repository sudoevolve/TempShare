param([Parameter(Mandatory = $true)][version]$InstalledVersion, [switch]$Automatic)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Net.Http
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = 'sudoevolve/TempShare'
$root = Join-Path $env:LOCALAPPDATA 'TempShare'
$secretPath = Join-Path $root 'github-update.credential'
$autoPath = Join-Path $root 'github-update.enabled'
$lastPath = Join-Path $root 'github-update.last'
$mutex = New-Object Threading.Mutex($false, 'Local\TempShare.Update')
try { $owned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
if (!$owned) { $mutex.Dispose(); exit }

function Message([string]$Text, [string]$Buttons = 'OK') {
    [Windows.Forms.MessageBox]::Show($Text, 'TempShare · GitHub', $Buttons, 'Information')
}

function Configure-Token {
    $form = New-Object Windows.Forms.Form
    $form.Text = 'TempShare · GitHub'
    $form.FormBorderStyle = 'FixedDialog'
    $form.StartPosition = 'CenterScreen'
    $form.ClientSize = New-Object Drawing.Size(440, 152)
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    try {
        $label = New-Object Windows.Forms.Label
        $label.Text = '私有仓库需要 GitHub Contents 读取令牌。留空保存可清除令牌。'
        $label.SetBounds(16, 12, 410, 35)
        $field = New-Object Windows.Forms.TextBox
        $field.UseSystemPasswordChar = $true
        $field.SetBounds(16, 55, 405, 28)
        $ok = New-Object Windows.Forms.Button
        $ok.Text = '保存'
        $ok.DialogResult = 'OK'
        $ok.SetBounds(264, 104, 73, 30)
        $cancel = New-Object Windows.Forms.Button
        $cancel.Text = '取消'
        $cancel.DialogResult = 'Cancel'
        $cancel.SetBounds(348, 104, 73, 30)
        $form.Controls.AddRange(@($label, $field, $ok, $cancel))
        $form.AcceptButton = $ok
        $form.CancelButton = $cancel
        if ($form.ShowDialog() -ne 'OK') { return '' }
        $value = $field.Text.Trim()
        if (!$value) {
            if (Test-Path -LiteralPath $secretPath) { Remove-Item -LiteralPath $secretPath }
            return ''
        }
        if ($value -notmatch '^[A-Za-z0-9_]{20,255}$') { throw 'Invalid GitHub token format' }
        $secure = ConvertTo-SecureString $value -AsPlainText -Force
        try { ConvertFrom-SecureString $secure | Set-Content -LiteralPath $secretPath -Encoding ASCII }
        finally { $secure.Dispose() }
        return $value
    } finally { $form.Dispose() }
}

function Headers([string]$Token, [string]$Accept) {
    $headers = @{ Accept = $Accept; 'X-GitHub-Api-Version' = '2022-11-28' }
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    return $headers
}

function Get-Release([string]$Token) {
    Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -UserAgent 'TempShare' -Headers (Headers $Token 'application/vnd.github+json') -TimeoutSec 15
}

function Save-Asset($Asset, [string]$Token, [string]$Path) {
    $handler = New-Object Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $client = New-Object Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(120)
    $client.MaxResponseContentBufferSize = [long]$Asset.size + 65536
    $uri = [Uri]"https://api.github.com/repos/$repo/releases/assets/$([long]$Asset.id)"
    try {
        for ($step = 0; $step -lt 4; $step++) {
            $request = New-Object Net.Http.HttpRequestMessage([Net.Http.HttpMethod]::Get, $uri)
            try {
                $request.Headers.UserAgent.ParseAdd('TempShare')
                foreach ($header in (Headers $Token 'application/octet-stream').GetEnumerator()) {
                    [void]$request.Headers.TryAddWithoutValidation($header.Key, $header.Value)
                }
                $response = $client.SendAsync($request).GetAwaiter().GetResult()
                try {
                    if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
                        $uri = [Uri]::new($uri, $response.Headers.Location)
                        if ($uri.Scheme -ne 'https' -or $uri.Host -notin @('release-assets.githubusercontent.com', 'objects.githubusercontent.com')) {
                            throw 'Unexpected GitHub asset redirect'
                        }
                        $Token = ''
                        continue
                    }
                    [void]$response.EnsureSuccessStatusCode()
                    $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
                    if ($bytes.LongLength -ne [long]$Asset.size) { throw 'Invalid asset size' }
                    [IO.File]::WriteAllBytes($Path, $bytes)
                    return
                } finally { $response.Dispose() }
            } finally { $request.Dispose() }
        }
        throw 'Too many GitHub redirects'
    } finally { $client.Dispose() }
}

$installer = $null
try {
    [void][IO.Directory]::CreateDirectory($root)
    if ($Automatic) {
        if (!(Test-Path -LiteralPath $autoPath)) { exit }
        if ((Test-Path -LiteralPath $lastPath) -and ((Get-Date) - (Get-Item -LiteralPath $lastPath).LastWriteTime).TotalHours -lt 24) { exit }
    } else {
        $choice = Message '检查 GitHub 上的新版本？「是」同时开启每天自动检查。「否」只检查一次并关闭自动检查。' 'YesNoCancel'
        if ($choice -eq 'Cancel') { exit }
        if ($choice -eq 'Yes') { [IO.File]::WriteAllText($autoPath, '') }
        elseif (Test-Path -LiteralPath $autoPath) { Remove-Item -LiteralPath $autoPath }
    }
    $token = ''
    if (Test-Path -LiteralPath $secretPath) {
        $secure = Get-Content -LiteralPath $secretPath | ConvertTo-SecureString
        try { $token = [Net.NetworkCredential]::new('', $secure).Password }
        finally { $secure.Dispose() }
    }
    [IO.File]::WriteAllText($lastPath, '')
    try { $release = Get-Release $token }
    catch {
        $code = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
        if ($Automatic -or $code -notin @(401, 403, 404)) { throw }
        $token = Configure-Token
        if (!$token) {
            if ((Message '打开 GitHub 发布页？请在浏览器登录有仓库权限的账号。' 'YesNo') -eq 'Yes') {
                Start-Process "https://github.com/$repo/releases"
            }
            exit
        }
        $release = Get-Release $token
    }
    if ($release.draft -or $release.prerelease -or $release.tag_name -notmatch '^v([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'No stable release' }
    $version = [version]$Matches[1]
    if ($version -le $InstalledVersion) {
        if (!$Automatic) { [void](Message "当前版本 $InstalledVersion 已是最新版本。") }
        exit
    }
    $name = "TempShare-$version-windows-x64.exe"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    if ($assets.Count -ne 1 -or $assets[0].digest -notmatch '^sha256:([0-9a-f]{64})$' -or
        $assets[0].size -lt 1 -or $assets[0].size -gt 100MB) { throw 'Missing verified installer' }
    $digest = $Matches[1]
    if ((Message "发现 TempShare $version。下载并安装？安装时当前共享会结束。" 'YesNo') -ne 'Yes') { exit }
    $installer = Join-Path $root ("update-$([Guid]::NewGuid().ToString('N')).exe")
    Save-Asset $assets[0] $token $installer
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $digest) { throw 'Installer hash mismatch' }
    Start-Process -FilePath $installer -Wait
} catch {
    if (!$Automatic) { [void](Message '无法获取 GitHub 更新。请检查网络、仓库读取权限和发布页面。') }
} finally {
    if ($installer -and (Test-Path -LiteralPath $installer)) { Remove-Item -LiteralPath $installer -ErrorAction SilentlyContinue }
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
