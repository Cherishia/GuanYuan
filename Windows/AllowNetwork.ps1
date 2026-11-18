$ErrorActionPreference = 'Stop'
try {
    $program = Join-Path $PSScriptRoot 'GuanYuan.exe'
    if (-not (Test-Path -LiteralPath $program)) { throw 'Please extract the complete Windows folder first.' }
    $program = (Resolve-Path -LiteralPath $program).Path
    foreach ($rule in @(@{Name='PalmPower-TCP';Protocol='TCP';Port=18677}, @{Name='PalmPower-UDP';Protocol='UDP';Port=18678})) {
        Get-NetFirewallRule -Name $rule.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        New-NetFirewallRule -Name $rule.Name -DisplayName ('PalmPower LAN ' + $rule.Protocol) -Direction Inbound -Action Allow -Program $program -Protocol $rule.Protocol -LocalPort $rule.Port -RemoteAddress LocalSubnet -Profile Any | Out-Null
    }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show('局域网连接已允许。现在可以在手机 App 中点击重新搜索。','关源') | Out-Null
} catch {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show(('设置失败：' + $_.Exception.Message),'关源') | Out-Null
    exit 1
}
