import * as constants from './constants.js';
import { Renderer, TextComponent } from './renderer.js';
import * as util from 'node:util';
// Global renderer so that multiple spinners can run at the same time
export const renderer = new Renderer();
const renderersByStream = new WeakMap([[process.stdout, renderer]]);
export class Spinner {
    running = false;
    text = '';
    currentSymbol;
    symbolFormatter;
    interval;
    frameIndex = 0;
    symbols;
    frames;
    component = new TextComponent('');
    colors;
    stream = process.stdout;
    outputRenderer = renderer;
    constructor(display = '', { disableNewLineEnding, colors, frames = constants.DEFAULT_FRAMES, symbols = {} } = {}) {
        // Merge symbols with defaults
        this.symbols = { ...constants.DEFAULT_SYMBOLS, ...symbols };
        // Setup colors if the option is passed
        if (typeof colors === 'object') {
            this.colors = {
                ...constants.DEFAULT_COLORS,
                ...colors
            };
        }
        else if (colors !== false) {
            this.colors = constants.DEFAULT_COLORS;
        }
        if (disableNewLineEnding === true)
            this.component.disableNewLineEnding();
        if (typeof display === 'string')
            display = { text: display };
        delete display.symbol;
        this.setDisplay(display, false);
        this.frames = frames;
        this.currentSymbol = frames[0];
    }
    start(tickMs = constants.DEFAULT_TICK_MS) {
        if (this.running)
            throw new Error('Spinner is already running.');
        if (this.component.finished)
            this.component.finished = false;
        this.interval = setInterval(this.tick.bind(this), tickMs);
        this.running = true;
        this.currentSymbol = this.frames[0];
        this.tick();
        this.outputRenderer.addComponent(this.component);
        this.addListeners();
    }
    tick() {
        this.currentSymbol = this.format(this.frames[this.frameIndex++], 'spinner');
        if (this.frameIndex === this.frames.length)
            this.frameIndex = 0;
        this.refresh();
    }
    onProcessExit = (signal) => {
        this.stop();
        // SIGTERM is 15, SIGINT is 2
        let signalCode;
        if (signal === 'SIGTERM') {
            signalCode = 15 + 128;
        }
        else if (signal === 'SIGINT') {
            signalCode = 2 + 128;
        }
        else {
            signalCode = Number(signal);
        }
        process.exit(signalCode);
    };
    addListeners() {
        process.once('SIGTERM', this.onProcessExit);
        process.once('SIGINT', this.onProcessExit);
        process.once('exit', this.onProcessExit);
    }
    clearListeners() {
        process.off('SIGTERM', this.onProcessExit);
        process.off('SIGINT', this.onProcessExit);
        process.off('exit', this.onProcessExit);
    }
    refresh() {
        let symbol = this.currentSymbol;
        if (this.symbolFormatter)
            symbol = this.symbolFormatter(symbol);
        const output = (symbol ? symbol + ' ' : '') + this.text;
        this.component.setText(output);
    }
    setDisplay(displayOpts = {}, render = true) {
        if (displayOpts.stream)
            this.setStream(displayOpts.stream);
        if (typeof displayOpts.symbol === 'string') {
            if (typeof displayOpts.symbolType === 'string') {
                this.currentSymbol = this.format(displayOpts.symbol, displayOpts.symbolType);
            }
            else
                this.currentSymbol = displayOpts.symbol;
        }
        if (typeof displayOpts.text === 'string')
            this.setText(displayOpts.text, false);
        if (displayOpts.symbolFormatter)
            this.symbolFormatter = displayOpts.symbolFormatter;
        if (render)
            this.refresh();
        if (typeof displayOpts.symbol === 'string')
            this.end();
    }
    setStream(stream) {
        if (stream === this.stream)
            return;
        const wasRunning = this.running;
        if (wasRunning)
            this.outputRenderer.removeComponent(this.component);
        this.stream = stream;
        let outputRenderer = renderersByStream.get(stream);
        if (!outputRenderer) {
            outputRenderer = new Renderer(true, stream);
            renderersByStream.set(stream, outputRenderer);
        }
        this.outputRenderer = outputRenderer;
        if (wasRunning)
            this.outputRenderer.addComponent(this.component);
    }
    setText(text, render = true) {
        this.text = this.format(text, 'text');
        if (this.running) {
            if (render)
                this.refresh();
        }
    }
    succeed(display) {
        if (typeof display === 'string')
            this.setDisplay({ text: display, symbol: this.symbols.succeed, symbolType: 'succeed' });
        else
            this.setDisplay({ ...display, symbol: this.symbols.succeed, symbolType: 'succeed' });
    }
    fail(display) {
        if (typeof display === 'string')
            this.setDisplay({ text: display, symbol: this.symbols.fail, symbolType: 'fail' });
        else
            this.setDisplay({ ...display, symbol: this.symbols.fail, symbolType: 'fail' });
    }
    warn(display) {
        if (typeof display === 'string')
            this.setDisplay({ text: display, symbol: this.symbols.warn, symbolType: 'warn' });
        else
            this.setDisplay({ ...display, symbol: this.symbols.warn, symbolType: 'warn' });
    }
    info(display) {
        if (typeof display === 'string')
            this.setDisplay({ text: display, symbol: this.symbols.info, symbolType: 'info' });
        else
            this.setDisplay({ ...display, symbol: this.symbols.info, symbolType: 'info' });
    }
    stop() {
        this.end(false);
    }
    end(keepComponent = true) {
        clearInterval(this.interval);
        this.clearListeners();
        if (keepComponent)
            this.component.finish();
        else
            this.outputRenderer.removeComponent(this.component);
        this.running = false;
    }
    format(component, componentName) {
        if (this.colors === undefined)
            return component;
        const color = this.colors[componentName];
        // Ensure that the Node version contains util.styleText
        if (color && util.styleText !== undefined) {
            return util.styleText(color, component);
        }
        return component;
    }
}
