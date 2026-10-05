$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ApolloUiNative {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")]
  public static extern IntPtr GetDlgItem(IntPtr parent, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool SetWindowText(IntPtr hwnd, string text);
  [DllImport("user32.dll")]
  public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")]
  public static extern bool IsWindowEnabled(IntPtr hwnd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int GetWindowTextLength(IntPtr hwnd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int capacity);
}
'@

$running = Get-Process apollo_1_0_chat -ErrorAction SilentlyContinue |
  Where-Object { $_.MainWindowHandle -ne 0 } |
  Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $running) { throw 'Apollo chat window is not open.' }
$window = [IntPtr]$running.MainWindowHandle
$prompt = [ApolloUiNative]::GetDlgItem($window, 101)
$send = [ApolloUiNative]::GetDlgItem($window, 102)
$output = [ApolloUiNative]::GetDlgItem($window, 103)
if ($prompt -eq [IntPtr]::Zero -or $send -eq [IntPtr]::Zero -or $output -eq [IntPtr]::Zero) {
  throw 'Apollo chat controls were not found.'
}
$existing = New-Object System.Text.StringBuilder 4096
[void][ApolloUiNative]::GetWindowText($prompt, $existing, $existing.Capacity)
$question = 'Apollo, design a better user interface for yourself. The current app is a native C Win32 chat window. Propose a practical, prioritized design that improves readability, navigation through conversation history, composing and sending messages, response status, and model/reference visibility. Explain the layout, controls, and keyboard behavior. Keep it realistic to implement in plain C Win32, concise enough that we can implement it, and do not write source code yet.'
if ($existing.Length -gt 0 -and $existing.ToString() -ne $question) {
  throw 'The chat input contains different text; leaving it untouched.'
}
$before = [ApolloUiNative]::GetWindowTextLength($output)
[void][ApolloUiNative]::SetWindowText($prompt, $question)
[void][ApolloUiNative]::SendMessage($window, 0x0111, [IntPtr]102, $send)
Start-Sleep -Milliseconds 700
$deadline = (Get-Date).AddSeconds(90)
$wasDisabled = $false
while ((Get-Date) -lt $deadline) {
  $enabled = [ApolloUiNative]::IsWindowEnabled($send)
  if (-not $enabled) { $wasDisabled = $true }
  if ($wasDisabled -and $enabled) { break }
  Start-Sleep -Milliseconds 500
}
$length = [ApolloUiNative]::GetWindowTextLength($output)
$capacity = [Math]::Min([Math]::Max($length + 1, 1), 60000)
$text = New-Object System.Text.StringBuilder $capacity
[void][ApolloUiNative]::GetWindowText($output, $text, $capacity)
$full = $text.ToString()
$tailStart = [Math]::Min($before, $full.Length)
$tail = $full.Substring($tailStart)
if (-not $wasDisabled -or -not [ApolloUiNative]::IsWindowEnabled($send)) {
  throw 'Apollo did not finish within 90 seconds.'
}
Set-Content -LiteralPath 'apollo_ui_design_response_temp.txt' -Value $tail -Encoding utf8
Write-Output $tail
