Get-NetFirewallRule -DisplayName 'Node.js JavaScript Runtime' | ForEach-Object {
  $p = ($_ | Get-NetFirewallApplicationFilter).Program
  $ports = ($_ | Get-NetFirewallPortFilter)
  "{0} | Enabled={1} | Profile={2} | Program={3} | Proto={4} LocalPort={5}" -f $_.DisplayName, $_.Enabled, $_.Profile, $p, $ports.Protocol, ($ports.LocalPort -join ',')
}
"---- block rules for node ----"
Get-NetFirewallApplicationFilter -Program '*node*' -ErrorAction SilentlyContinue | Get-NetFirewallRule | Where-Object { $_.Action -eq 'Block' } | ForEach-Object { "{0} | {1} | {2}" -f $_.DisplayName, $_.Profile, $_.Enabled }
