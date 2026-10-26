# SPDX-License-Identifier: GPL-2.0-only
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

Add-Type -AssemblyName System.Windows.Forms
$Form = New-Object Windows.Forms.Form
$Form.Text = 'FPLinux USB driver setup'
$Form.ClientSize = '640,370'
$Form.StartPosition = 'CenterScreen'
$Form.TopMost = $true
$Form.FormBorderStyle = 'FixedDialog'
$Form.MaximizeBox = $false
$Form.MinimizeBox = $false

$Label = New-Object Windows.Forms.Label
$Label.Location = '16,16'
$Label.Size = '608,210'
$Label.Text = (
    "Set up USB access for the phone?`r`n`r`n" +
    "Administrator approval is required. Setup will:`r`n`r`n" +
    "- Replace the driver for BootROM phones with USB ID 1782:4D00 with Windows WinUSB.`r`n`r`n" +
    "- Add a local driver-signing certificate to this PC's trusted certificate stores (Root and Trusted Publishers). Its signing key is deleted after signing.`r`n`r`n" +
    '- Pause restore-point creation during installation and restore it when the installer exits normally.'
)

$Accept = New-Object Windows.Forms.CheckBox
$Accept.Location = '16,242'
$Accept.Size = '608,26'
$Accept.Text = 'I authorize these driver and certificate changes.'
$Install = New-Object Windows.Forms.Button
$Install.Location = '390,310'
$Install.Size = '112,32'
$Install.Text = 'Install'
$Install.Enabled = $false
$Install.DialogResult = [Windows.Forms.DialogResult]::OK
$Cancel = New-Object Windows.Forms.Button
$Cancel.Location = '512,310'
$Cancel.Size = '112,32'
$Cancel.Text = 'Cancel'
$Cancel.DialogResult = [Windows.Forms.DialogResult]::Cancel
$Form.CancelButton = $Cancel
$Accept.Add_CheckedChanged({ $Install.Enabled = $Accept.Checked })
$Form.Controls.AddRange(@($Label, $Accept, $Install, $Cancel))

try {
    $Choice = $Form.ShowDialog()
    $Result = @{
        accepted = ($Choice -eq [Windows.Forms.DialogResult]::OK -and $Accept.Checked)
    }
} finally {
    $Form.Dispose()
}
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
