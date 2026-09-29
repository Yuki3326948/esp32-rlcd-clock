<#
 * build.ps1 —— 把编译固定在性能核(P 核)上,避开节能核(E 核)。
 *
 * 为什么需要这么做
 *   Windows 会把后台、短命的进程优先丢到 E 核上。而一次 idf.py build
 *   要起几百个 ccache / gcc / ninja 之类的子进程,全都落在 E 核的话
 *   整体编译时间明显变长。
 *
 *   进程的 CPU 亲和性会被子进程继承,所以只要在启动构建前把自己的
 *   亲和性限制一次,后面整棵树都跟着走 —— 不用逐个进程去设。
 *
 * 大小核数量是怎么算出来的(不靠猜)
 *     P + E  = 物理核数       NumberOfCores
 *     2P + E = 逻辑处理器数   NumberOfLogicalProcessors(P 核有超线程,E 核没有)
 *   => P = 逻辑数 - 物理数
 *   而固件把 P 核排在逻辑编号的前面,所以 P 核 = 逻辑 0 .. 2P-1。
 *
 *   这台机器:10 核 16 线程 -> P = 16-10 = 6,E = 4
 *   所以 P 核是逻辑 0~11,掩码 0xFFF。
 *
 * 用法(在工程目录下)
 *     .\build.ps1              # 等价于 idf.py build
 *     .\build.ps1 -p COM5 flash
 *     .\build.ps1 fullclean
 *     .\build.ps1 -ShowOnly    # 只看识别结果,不做别的
 *
 * 注意:如果哪天换了台机器,这个推导会自动重算,不用改脚本。
 *       只有遇到 E 核也带超线程的型号才需要手动传 -Mask。
#>

# 故意【不声明 param 块】:
#   idf.py 自己也要用 -p(串口),而 PowerShell 的公共参数里有 -PipelineVariable,
#   声明了 param 就会把 -p 报成"参数名称存在歧义"。不声明就不会做参数绑定,
#   所有参数原样落进 $args,想过什么就过什么。
$IdfArgs = @()
$Mask    = $null
$ShowOnly = $false

for ($i = 0; $i -lt $args.Count; $i++) {
    switch -Regex ($args[$i]) {
        '^-ShowOnly$' { $ShowOnly = $true;  continue }
        '^-Mask$'     { $Mask = $args[$i + 1]; $i++; continue }
        default       { $IdfArgs += $args[$i] }
    }
}
if ($IdfArgs.Count -eq 0) { $IdfArgs = @('build') }

$ErrorActionPreference = 'Stop'

# ---------- 1. 算出 P 核掩码 ----------
if ($Mask) {
    $hex = $Mask -replace '^0[xX]', ''
} else {
    $cpu   = Get-CimInstance Win32_Processor | Select-Object -First 1
    $phys  = [int]$cpu.NumberOfCores
    $logic = [int]$cpu.NumberOfLogicalProcessors

    $pCoreCount = $logic - $phys          # 见文件头推导
    if ($pCoreCount -le 0 -or $pCoreCount -ge $logic) {
        Write-Warning "核心布局看不出大小核(P=$pCoreCount,E=$($phys-$pCoreCount)),不改亲和性。"
        $hex = $null
    } else {
        $pThreads = 2 * $pCoreCount
        $hex = '{0:X}' -f [int]((1L -shl $pThreads) - 1)
        "CPU      : $($cpu.Name)"
        "物理核   : $phys   (P=$pCoreCount, E=$($phys - $pCoreCount))"
        "逻辑处理器: $logic"
        "P 核线程 : 逻辑 0..$($pThreads - 1)  ->  亲和性掩码 0x$hex"
    }
}

# ---------- 2. 把自己钉到 P 核,并提高优先级 ----------
if ($ShowOnly) { return }

if ($hex) {
    $proc = [System.Diagnostics.Process]::GetCurrentProcess()
    $proc.ProcessorAffinity = [IntPtr][Convert]::ToInt64($hex, 16)
    try { $proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High } catch {}
    "已生效   : 亲和性=0x$hex  优先级=High"
    "          (子进程 ccache/gcc/ninja 会继承,不用逐个设)"
}

# ---------- 3. 装好 IDF 环境再编译 ----------
$profile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (Test-Path $profile) {
    . $profile | Out-Null
}
$env:PYTHONUTF8 = '1'          # Windows 上不加这个,IDF 的 Python 会拿 GBK 解码 UTF-8 输出而报错

"---- idf.py $($IdfArgs -join ' ') ----"
& idf.py @IdfArgs
