define("dojox/charting/Chart3D", ["dojo/_base/array", "dojo/dom","dojo/_base/declare", "dojox/gfx", "dojox/gfx3d",
        "dojo/has", "dojo/has!dojo-bidi?./bidi/Chart3D"],
	function(arr, dom, declare, gfx, gfx3d, has, BidiChart3D){
	// module:
	//		dojox/charting/Chart3D
	// summary:
	//		This module provides basic 3d charting capablities (using 2d vector graphics to simulate 3d.

	/*=====
	var __Chart3DCtorArgs = function(node, lights, camera, theme){
		// summary:
		//		The keyword arguments that can be passed in a Chart constructor.
		//
		// node: Node
		//		The DOM node to construct the chart on.
		// lights:
		//		Lighting properties for the 3d scene
		// camera: Object
		//		Camera properties describing the viewing camera position.
		// theme: Object
		//		Charting theme to use for coloring chart elements.
	};
	=====*/
	var observerVector = {x: 0, y: 0, z: 1}, v = gfx3d.vector, n = gfx.normalizedLength;

	var Chart3D = declare(has("dojo-bidi")? "dojox.charting.NonBidiChart3D" : "dojox.charting.Chart3D", null, {
		constructor: function(node, lights, camera, theme){
			// setup a view
			this.node = dom.byId(node);
			this.surface = gfx.createSurface(this.node, n(this.node.style.width), n(this.node.style.height));
			this.view = this.surface.createViewport();
			this.view.setLights(lights.lights, lights.ambient, lights.specular);
			this.view.setCameraTransform(camera);
			this.theme = theme;
			
			// initialize internal variables
			this.walls = [];
			this.plots = [];
		},
		
		// public API
		generate: function(){
			return this._generateWalls()._generatePlots();
		},
		invalidate: function(){
			this.view.invalidate();
			return this;
		},
		render: function(){
			this.view.render();
			return this;
		},
		addPlot: function(plot){
			return this._add(this.plots, plot);
		},
		removePlot: function(plot){
			return this._remove(this.plots, plot);
		},
		addWall: function(wall){
			return this._add(this.walls, wall);
		},
		removeWall: function(wall){
			return this._remove(this.walls, wall);
		},
		
		// internal API
		_add: function(array, item){
			if(!arr.some(array, function(i){ return i == item; })){
				array.push(item);
				this.view.invalidate();
			}
			return this;
		},
		_remove: function(array, item){
			var a = arr.filter(array, function(i){ return i != item; });
			return a.length < array.length ? (array = a, this.invalidate()) : this;
		},
		_generateWalls: function(){
			for(var i = 0; i < this.walls.length; ++i){
				if(v.dotProduct(observerVector, this.walls[i].normal) > 0){
					this.walls[i].generate(this);
				}
			}
			return this;
		},
		_generatePlots: function(){
			var depth = 0, m = gfx3d.matrix, i = 0;
			for(; i < this.plots.length; ++i){
				depth += this.plots[i].getDepth();
			}
			for(--i; i >= 0; --i){
				var scene = this.view.createScene();
				scene.setTransform(m.translate(0, 0, -depth));
				this.plots[i].generate(this, scene);
				depth -= this.plots[i].getDepth();
			}
			return this;
		},
		setDir: function(/*String*/dir){
			return this;
		}
	});
	return has("dojo-bidi")? declare("dojox.charting.Chart3D", [Chart3D, BidiChart3D]) : Chart3D;
});
