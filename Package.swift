// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "SeeEverything",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "SeeEverything", targets: ["SeeEverything"]),
        .executable(name: "see-cli", targets: ["SeeCLI"])
    ],
    targets: [
        .target(
            name: "EverythingCore",
            path: "Sources/EverythingCore",
            publicHeadersPath: "include",
            cxxSettings: [
                .unsafeFlags(["-std=c++20", "-O3", "-Wall", "-Wextra", "-Wno-unused-parameter"])
            ],
            linkerSettings: [
                .linkedFramework("CoreServices"),
                .linkedFramework("CoreFoundation")
            ]
        ),
        .executableTarget(
            name: "SeeEverything",
            dependencies: ["EverythingCore"],
            path: "Sources/SeeEverything",
            swiftSettings: [
                .swiftLanguageMode(.v5)
            ],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Carbon"),
                .linkedFramework("QuickLookUI"),
                .linkedFramework("Quartz"),
                .linkedFramework("UniformTypeIdentifiers")
            ]
        ),
        .executableTarget(
            name: "SeeCLI",
            dependencies: ["EverythingCore"],
            path: "Sources/SeeCLI",
            swiftSettings: [
                .swiftLanguageMode(.v5)
            ]
        )
    ],
    cxxLanguageStandard: .cxx20
)
