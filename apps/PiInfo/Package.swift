// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "PiInfo",
    platforms: [
        // The Pi runs iPadOS 16.6.1.
        .iOS(.v16),
        .macOS(.v14),
    ],
    products: [
        // An xtool project should contain exactly one library product,
        // representing the main app.
        .library(
            name: "PiInfo",
            targets: ["PiInfo"]
        ),
    ],
    targets: [
        .target(
            name: "PiInfo"
        ),
    ]
)
