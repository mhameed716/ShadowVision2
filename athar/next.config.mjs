/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // Allowed dev-server origins for the sandboxed preview host.
  allowedDevOrigins: ['*.e2b.app', 'localhost', '127.0.0.1'],
  experimental: {
    serverActions: { bodySizeLimit: '2mb' }
  }
};

export default nextConfig;
