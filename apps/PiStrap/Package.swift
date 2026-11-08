// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "PiStrap",
    platforms: [
        // The Pi runs iPadOS 16.6.1.
        .iOS(.v16),
        .macOS(.v14),
    ],
    products: [
        // An xtool project should contain exactly one library product,
        // representing the main app.
        .library(
            name: "PiStrap",
            targets: ["PiStrap"]
        ),
    ],
    targets: [
        .target(
            name: "PiStrap"
        ),
    ]
)
