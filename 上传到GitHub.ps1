# foo_spectrum_compare 一键上传到 GitHub 脚本
# 使用方法：
# 1. 安装 Git for Windows (https://git-scm.com/download/win)
# 2. 在 GitHub 创建空仓库，复制仓库地址
# 3. 修改下面 $repoUrl 为你的仓库地址
# 4. 右键此文件 -> "使用 PowerShell 运行"
# 5. 按提示输入 GitHub 用户名和密码（或 Personal Access Token）

# ============== 请修改这里 ==============
$repoUrl = "https://github.com/a1waysbeta/foo_spectrum_compare.git"
# =========================================

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  foo_spectrum_compare 一键上传脚本" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# 检查 Git 是否安装
try {
    $gitVersion = git --version
    Write-Host "[OK] Git 已安装: $gitVersion" -ForegroundColor Green
} catch {
    Write-Host "[错误] 未检测到 Git，请先安装 Git for Windows" -ForegroundColor Red
    Write-Host "下载地址: https://git-scm.com/download/win" -ForegroundColor Yellow
    Write-Host "安装后请重新运行此脚本"
    Read-Host "按回车键退出"
    exit 1
}

# 检查仓库地址是否已修改
if ($repoUrl -match "你的用户名") {
    Write-Host "[错误] 请先修改脚本中的 `$repoUrl 为你的 GitHub 仓库地址" -ForegroundColor Red
    Write-Host "用记事本打开此文件，找到 '你的用户名' 替换为你的 GitHub 用户名"
    Read-Host "按回车键退出"
    exit 1
}

Write-Host "[信息] 目标仓库: $repoUrl" -ForegroundColor Yellow
Write-Host ""

# 切换到脚本所在目录
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir
Write-Host "[信息] 当前目录: $scriptDir" -ForegroundColor Yellow
Write-Host ""

# 初始化 Git 仓库（如果还没有）
if (-not (Test-Path ".git")) {
    Write-Host "[1/5] 初始化 Git 仓库..." -ForegroundColor Cyan
    git init
    git branch -M main
} else {
    Write-Host "[1/5] Git 仓库已存在，跳过初始化" -ForegroundColor Cyan
}

# 配置 Git 用户信息（如果还没有配置）
$userName = git config user.name 2>$null
$userEmail = git config user.email 2>$null
if (-not $userName) {
    Write-Host ""
    Write-Host "[配置] 首次使用 Git，需要设置用户名和邮箱" -ForegroundColor Yellow
    $gitName = Read-Host "请输入你的 GitHub 用户名"
    $gitEmail = Read-Host "请输入你的 GitHub 邮箱"
    git config user.name $gitName
    git config user.email $gitEmail
    Write-Host "[OK] Git 用户信息已配置" -ForegroundColor Green
}

# 添加所有文件
Write-Host ""
Write-Host "[2/5] 添加文件到暂存区..." -ForegroundColor Cyan
git add .
$fileCount = (git diff --cached --name-only | Measure-Object -Line).Lines
Write-Host "[OK] 已添加 $fileCount 个文件" -ForegroundColor Green

# 提交
Write-Host ""
Write-Host "[3/5] 提交文件..." -ForegroundColor Cyan
git commit -m "Initial commit: foo_spectrum_compare foobar2000 component"
Write-Host "[OK] 提交完成" -ForegroundColor Green

# 添加远程仓库
$existingRemote = git remote get-url origin 2>$null
if ($existingRemote) {
    Write-Host ""
    Write-Host "[信息] 已存在远程仓库: $existingRemote" -ForegroundColor Yellow
    $update = Read-Host "是否更新为新地址? (y/n)"
    if ($update -eq "y" -or $update -eq "Y") {
        git remote set-url origin $repoUrl
        Write-Host "[OK] 远程仓库地址已更新" -ForegroundColor Green
    }
} else {
    Write-Host ""
    Write-Host "[4/5] 添加远程仓库..." -ForegroundColor Cyan
    git remote add origin $repoUrl
    Write-Host "[OK] 远程仓库已添加" -ForegroundColor Green
}

# 推送到 GitHub
Write-Host ""
Write-Host "[5/5] 推送到 GitHub..." -ForegroundColor Cyan
Write-Host "[提示] 如果提示输入密码，请输入 GitHub 密码或 Personal Access Token" -ForegroundColor Yellow
Write-Host "[提示] 如果你开启了两步验证，必须使用 Personal Access Token 代替密码" -ForegroundColor Yellow
Write-Host ""

try {
    git push -u origin main
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  上传成功！" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "接下来：" -ForegroundColor Cyan
    Write-Host "1. 打开你的 GitHub 仓库页面"
    Write-Host "2. 点击顶部的 Actions 标签，查看自动编译进度"
    Write-Host "3. 编译完成后（约2-3分钟），在工作流详情页底部下载 Artifacts"
    Write-Host ""
} catch {
    Write-Host ""
    Write-Host "[错误] 推送失败，请检查：" -ForegroundColor Red
    Write-Host "1. 仓库地址是否正确"
    Write-Host "2. GitHub 用户名和密码/Token 是否正确"
    Write-Host "3. 网络连接是否正常"
    Write-Host ""
    Write-Host "如果开启了两步验证，需要使用 Personal Access Token："
    Write-Host "GitHub -> Settings -> Developer settings -> Personal access tokens -> Generate new token"
    Write-Host "勾选 repo 权限，生成后用 Token 代替密码"
}

Write-Host ""
Read-Host "按回车键退出"
