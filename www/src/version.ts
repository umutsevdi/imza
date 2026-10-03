// The pipeline sets IMZA_VERSION for the site build, mirroring the
// IMZA_VERSION compile definition the app gets from CMake.
export const VERSION = process.env.IMZA_VERSION || "v0.0.0";
