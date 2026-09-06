# Generates res/app.ico  -  run once; the .ico is checked in.
Add-Type -AssemblyName System.Drawing

function New-Glyph([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded body
    $pad = [Math]::Max(0.0, $S * 0.02)
    $r   = [double]($S * 0.235)
    $x0 = $pad; $y0 = $pad; $x1 = $S - $pad; $y1 = $S - $pad
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x0, $y0, $d, $d, 180, 90)
    $path.AddArc($x1 - $d, $y0, $d, $d, 270, 90)
    $path.AddArc($x1 - $d, $y1 - $d, $d, $d, 0, 90)
    $path.AddArc($x0, $y1 - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 44, 47, 54)
    $c2 = [System.Drawing.Color]::FromArgb(255, 20, 21, 25)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            (New-Object System.Drawing.PointF(0, 0)),
            (New-Object System.Drawing.PointF(0, [float]$S)), $c1, $c2)
    $g.FillPath($br, $path)

    # hairline highlight on the top edge
    if ($S -ge 32) {
        $pen = New-Object System.Drawing.Pen(
            [System.Drawing.Color]::FromArgb(46, 255, 255, 255), [float]([Math]::Max(1.0, $S * 0.012)))
        $g.DrawPath($pen, $path); $pen.Dispose()
    }

    # crop brackets
    $t   = [Math]::Max(1.0, $S * 0.072)          # stroke
    $m   = $S * 0.235                            # margin from edge
    $len = $S * 0.150                            # arm length
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [float]$t)
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
    $lo = $m; $hi = $S - $m
    foreach ($c in @(@($lo,$lo,1,1), @($hi,$lo,-1,1), @($lo,$hi,1,-1), @($hi,$hi,-1,-1))) {
        $cx = $c[0]; $cy = $c[1]; $sx = $c[2]; $sy = $c[3]
        $pts = @(
            (New-Object System.Drawing.PointF([float]($cx),           [float]($cy + $sy*$len))),
            (New-Object System.Drawing.PointF([float]($cx),           [float]($cy))),
            (New-Object System.Drawing.PointF([float]($cx + $sx*$len),[float]($cy)))
        )
        $g.DrawLines($pen, $pts)
    }

    # accent dot
    $ds = $S * 0.160
    $ab = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 76, 141, 255))
    $g.FillEllipse($ab, [float](($S - $ds)/2), [float](($S - $ds)/2), [float]$ds, [float]$ds)

    $ab.Dispose(); $pen.Dispose(); $br.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# ---- pack into .ico -------------------------------------------------
$sizes   = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$entries = @()
foreach ($s in $sizes) {
    $bmp = New-Glyph $s
    if ($s -ge 128) {                                    # PNG-compressed entry
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $data = $ms.ToArray(); $ms.Dispose()
    } else {                                             # classic BGRA DIB entry
        $rowXor = $s * 4
        $rowAnd = [int]([Math]::Floor(($s + 31) / 32)) * 4
        $data = New-Object byte[] (40 + $rowXor*$s + $rowAnd*$s)
        $bw = New-Object System.IO.BinaryWriter (New-Object System.IO.MemoryStream($data, $true))
        $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s*2))
        $bw.Write([uint16]1);  $bw.Write([uint16]32);  $bw.Write([uint32]0)
        $bw.Write([uint32]($rowXor*$s)); $bw.Write([int32]0); $bw.Write([int32]0)
        $bw.Write([uint32]0); $bw.Write([uint32]0)
        $bd = $bmp.LockBits((New-Object System.Drawing.Rectangle(0,0,$s,$s)),
                            'ReadOnly', 'Format32bppArgb')
        $row = New-Object byte[] $rowXor
        for ($y = $s - 1; $y -ge 0; $y--) {              # bottom-up
            [System.Runtime.InteropServices.Marshal]::Copy(
                [IntPtr]::Add($bd.Scan0, $y * $bd.Stride), $row, 0, $rowXor)
            $bw.Write($row, 0, $rowXor)
        }
        $bmp.UnlockBits($bd)
        $bw.Flush()
    }
    $entries += ,@($s, $data)
    $bmp.Dispose()
}

$out = New-Object System.IO.MemoryStream
$w   = New-Object System.IO.BinaryWriter($out)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $s = $e[0]; $d = $e[1]
    $w.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$d.Length); $w.Write([uint32]$offset)
    $offset += $d.Length
}
foreach ($e in $entries) { $w.Write($e[1]) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'app.ico'), $out.ToArray())
$out.Dispose()
Write-Host "app.ico written ($((Get-Item (Join-Path $PSScriptRoot 'app.ico')).Length) bytes)"
