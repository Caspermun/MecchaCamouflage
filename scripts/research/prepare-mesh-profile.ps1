param(
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$MeshPath = "",
    [string]$OutPath = "",
    [int]$TextureSize = 1024
)

$ErrorActionPreference = "Stop"

if (-not $MeshPath) {
    $MeshPath = Join-Path $RuntimeRoot ".build\research\mesh_exports\paintman-Chameleon_Content_3Dmodel_cLeon_charactor_paintman_skeltal_paintman.uasset.lod0.json"
}
if (-not $OutPath) {
    $OutPath = Join-Path $RuntimeRoot ".build\bin\mesh-profiles\paintman.mesh-profile-v2.json"
}
if (-not (Test-Path $MeshPath -PathType Leaf)) {
    throw "Mesh export not found: $MeshPath. Run scripts\research\run-asset-probe.ps1 with -ExportTopSkeletal first."
}

function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace "-", "").ToLowerInvariant()
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
}

function Find-Root([int[]]$Parent, [int]$Index) {
    $current = $Index
    while ($Parent[$current] -ne $current) {
        $Parent[$current] = $Parent[$Parent[$current]]
        $current = $Parent[$current]
    }
    return $current
}

function Join-Set([int[]]$Parent, [int]$A, [int]$B) {
    $ra = Find-Root $Parent $A
    $rb = Find-Root $Parent $B
    if ($ra -ne $rb) {
        $Parent[$rb] = $ra
    }
}

function Get-BodyRegion([string]$BoneName) {
    $name = $BoneName.ToLowerInvariant()
    if ($name -match "head|neck|face") { return "head" }
    if ($name -match "hand|arm|shoulder|elbow|wrist") { return "arm" }
    if ($name -match "leg|thigh|calf|foot|toe|knee") { return "leg" }
    if ($name -match "spine|chest|pelvis|hip|loot|amm") { return "torso" }
    return "unknown"
}

function Get-DominantBone($Vertices, [int[]]$TriangleIndices) {
    $weights = @{}
    foreach ($vertexIndex in $TriangleIndices) {
        $vertex = $Vertices[$vertexIndex]
        foreach ($influence in @($vertex.Influences)) {
            $bone = [int]$influence.Bone
            $weight = [double]$influence.Weight
            if ($bone -lt 0 -or $weight -le 0.0) { continue }
            if (-not $weights.ContainsKey($bone)) { $weights[$bone] = 0.0 }
            $weights[$bone] += $weight
        }
    }
    if ($weights.Count -eq 0) { return -1 }
    return [int](($weights.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key)
}

function Get-Vector($A, $B) {
    return @{
        X = [double]$B.X - [double]$A.X
        Y = [double]$B.Y - [double]$A.Y
        Z = [double]$B.Z - [double]$A.Z
    }
}

function Get-Cross($A, $B) {
    return @{
        X = ($A.Y * $B.Z) - ($A.Z * $B.Y)
        Y = ($A.Z * $B.X) - ($A.X * $B.Z)
        Z = ($A.X * $B.Y) - ($A.Y * $B.X)
    }
}

function Normalize-Vector($V) {
    $len = [Math]::Sqrt(($V.X * $V.X) + ($V.Y * $V.Y) + ($V.Z * $V.Z))
    if ($len -le 0.000001 -or [double]::IsNaN($len) -or [double]::IsInfinity($len)) {
        return @{ X = 0.0; Y = 0.0; Z = 0.0 }
    }
    return @{ X = $V.X / $len; Y = $V.Y / $len; Z = $V.Z / $len }
}

function Get-UvArea($A, $B, $C) {
    return [Math]::Abs((([double]$B.U - [double]$A.U) * ([double]$C.V - [double]$A.V)) - (([double]$C.U - [double]$A.U) * ([double]$B.V - [double]$A.V))) * 0.5
}

$profile = Get-Content -Raw -Path $MeshPath | ConvertFrom-Json
if ($profile.Export -ne "paintman") {
    throw "Unexpected mesh export '$($profile.Export)'. Expected 'paintman'."
}
if ($profile.Lod0.VertexCount -ne 1660 -or $profile.Lod0.IndexCount -ne 8352 -or $profile.Bones.Count -ne 28) {
    throw "Unexpected paintman profile shape. Expected vertices=1660 indices=8352 bones=28."
}
if ($TextureSize -lt 1) {
    throw "TextureSize must be positive."
}

$vertices = @($profile.Lod0.Vertices)
$indices = @($profile.Lod0.Indices | ForEach-Object { [int]$_ })
$triangleCount = [int]($indices.Count / 3)
if (($triangleCount * 3) -ne $indices.Count) {
    throw "Index count is not divisible by 3."
}

$parents = New-Object int[] $triangleCount
for ($i = 0; $i -lt $triangleCount; $i++) { $parents[$i] = $i }
$edgeOwners = @{}
for ($tri = 0; $tri -lt $triangleCount; $tri++) {
    $i0 = $indices[$tri * 3]
    $i1 = $indices[$tri * 3 + 1]
    $i2 = $indices[$tri * 3 + 2]
    foreach ($edge in @(@($i0, $i1), @($i1, $i2), @($i2, $i0))) {
        $a = [Math]::Min($edge[0], $edge[1])
        $b = [Math]::Max($edge[0], $edge[1])
        $key = "$a/$b"
        if ($edgeOwners.ContainsKey($key)) {
            Join-Set $parents $tri ([int]$edgeOwners[$key])
        } else {
            $edgeOwners[$key] = $tri
        }
    }
}

$rootToIsland = @{}
$nextIsland = 0
$triangles = New-Object System.Collections.Generic.List[object]
for ($tri = 0; $tri -lt $triangleCount; $tri++) {
    $i0 = $indices[$tri * 3]
    $i1 = $indices[$tri * 3 + 1]
    $i2 = $indices[$tri * 3 + 2]
    $v0 = $vertices[$i0]
    $v1 = $vertices[$i1]
    $v2 = $vertices[$i2]
    $root = Find-Root $parents $tri
    if (-not $rootToIsland.ContainsKey($root)) {
        $rootToIsland[$root] = $nextIsland
        $nextIsland++
    }
    $dominantBone = Get-DominantBone $vertices @($i0, $i1, $i2)
    $bodyRegion = "unknown"
    if ($dominantBone -ge 0 -and $dominantBone -lt $profile.Bones.Count) {
        $bodyRegion = Get-BodyRegion ([string]$profile.Bones[$dominantBone].Name)
    }
    $edge0 = Get-Vector $v0 $v1
    $edge1 = Get-Vector $v0 $v2
    $normal = Normalize-Vector (Get-Cross $edge0 $edge1)
    $triangles.Add([pscustomobject]@{
        Index = $tri
        I0 = $i0
        I1 = $i1
        I2 = $i2
        UvIsland = [int]$rootToIsland[$root]
        BodyRegion = $bodyRegion
        DominantBone = $dominantBone
        LocalNormalX = [double]$normal.X
        LocalNormalY = [double]$normal.Y
        LocalNormalZ = [double]$normal.Z
        UvArea = [double](Get-UvArea $v0 $v1 $v2)
    }) | Out-Null
}

$hash = Get-FileSha256 $MeshPath
$profileId = "$($profile.Export):$($profile.SourcePath):lod0:v$($profile.Lod0.VertexCount):i$($profile.Lod0.IndexCount):b$($profile.Bones.Count):$hash"
$out = [ordered]@{
    ProfileSchemaVersion = 2
    SchemaVersion = 2
    ProfileId = $profileId
    ProfileHash = $hash
    SourcePath = $profile.SourcePath
    Export = $profile.Export
    TextureSize = $TextureSize
    VertexCount = $profile.Lod0.VertexCount
    IndexCount = $profile.Lod0.IndexCount
    TriangleCount = $triangleCount
    UvIslandCount = $nextIsland
    Bones = $profile.Bones
    Lod0 = $profile.Lod0
    Triangles = $triangles
}

$outDir = Split-Path -Parent $OutPath
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
($out | ConvertTo-Json -Depth 80) | Set-Content -Encoding UTF8 -Path $OutPath
Write-Host "Prepared Mesh Profile V2: $OutPath"
