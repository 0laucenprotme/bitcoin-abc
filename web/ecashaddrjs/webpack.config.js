const path = require('path');
const webpack = require('webpack');
const pkg = require('./package.json');

const base = {
    mode: 'production',
    entry: path.resolve(__dirname, 'src', 'cashaddr.js'),
    output: {
        path: path.resolve(__dirname, 'dist'),
        library: 'cashaddr',
        libraryTarget: 'umd',
        globalObject: 'this',
    },
    resolve: {
        fallback: {
            buffer: require.resolve('buffer'),
        },
    },
    module: {
        rules: [
            {
                test: /\.js$/,
                exclude: /node_modules/,
                use: {
                    loader: 'babel-loader',
                    options: {
                        presets: [
                            ['@babel/preset-env', { targets: 'defaults' }],
                        ],
                    },
                },
            },
        ],
    },
    plugins: [
        // Work around for Buffer is undefined:
        // https://github.com/webpack/changelog-v5/issues/10
        new webpack.ProvidePlugin({
            Buffer: ['buffer', 'Buffer'],
        }),
    ],
};

module.exports = [
    Object.assign({}, base, {
        output: Object.assign({}, base.output, {
            filename: 'cashaddrjs.js',
        }),
        optimization: {
            minimize: false,
        },
    }),
    Object.assign({}, base, {
        output: Object.assign({}, base.output, {
            filename: 'cashaddrjs.min.js',
        }),
    }),
];
